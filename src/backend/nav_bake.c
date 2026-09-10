/* nav_bake.c -- see nav_bake.h for what this is and why it bakes at load. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <math.h>

#include "nav_bake.h"
#include "nav_regions.h"
#include "aas_edit.h"
#include "aas_augment.h"
#include "nav_traversal.h"
#include "navmesh.h"
#include "config.h"

#ifndef SH_NAV_BAKE_NO_LOG
void backend_log(const char *message);
#endif

/* The three demon size classes BuildAAS composes, in the order it asks. */
static const char *const NAV_CLASSES[] = { "monster48", "monster96", "monster128" };
#define NAV_CLASS_COUNT ((int)(sizeof NAV_CLASSES / sizeof NAV_CLASSES[0]))

/* The bake line carries the counts plus every note that applies -- islands,
 * side faces, cut volumes, dead-zone pairs -- and at 192 the notes were being
 * silently cut off mid-sentence, which reads to an author exactly like the bake
 * had nothing more to say. 64 modules at this size is 24 KB of statics. */
#define BAKE_REASON_CAP 384

typedef struct bake_module {
    char module[SH_NAVR_MODULE_CAP];    /* "category/module" */
    int  instance;                      /* the one instance carrying regions */
    int  regions;
    int  ok;
    char reason[BAKE_REASON_CAP];
} bake_module;

#define BAKE_MAX_MODULES SH_NAVR_MAX_INSTANCES

static SRWLOCK      g_bake_lock = SRWLOCK_INIT;
static sh_nav_map   g_map;
static bake_module  g_modules[BAKE_MAX_MODULES];
static int          g_module_count;
static int          g_have_map;
static volatile LONG g_bakes;
static volatile LONG g_faulted;
/* What the last pre-build live read actually saw. This is the one thing an author
 * cannot otherwise tell apart: "I ticked a box and nothing happened" has a very
 * different cause when the read never ran than when it ran and found nothing. */
static int g_live_scanned = -1;   /* entity ids offered, -1 = never ran */
static int g_live_marked;         /* marked volumes it attributed */
static int g_live_refused;        /* the read declined to commit */

static void bake_plan_locked(void);

/* The live-editor surface, registered once at startup. */
static sh_nav_bake_entity_count g_live_count;
static sh_navr_entity_valid     g_live_valid;
static sh_navr_entity_json      g_live_json;
static void                    *g_live_ctx;
static sh_nav_bake_snapshot g_snapshot;
static void *g_snapshot_ctx;
static volatile LONG g_building;
static unsigned long g_geometry_revision;
typedef struct bake_preview_cache {
    unsigned char *bytes;
    size_t length;
    unsigned first_area;
} bake_preview_cache;
static bake_preview_cache g_preview[BAKE_MAX_MODULES];
static unsigned long g_preview_revision = ~0UL;
static int g_preview_lines;
static int g_instance_serving;

void sh_nav_bake_enable_instances(int enabled) { g_instance_serving=enabled; }

static void bake_preview_clear(void)
{
    int i;
    for(i=0;i<BAKE_MAX_MODULES;i++) {
        if(g_preview[i].bytes)HeapFree(GetProcessHeap(),0,g_preview[i].bytes);
    }
    memset(g_preview,0,sizeof g_preview);
    g_preview_revision=~0UL;g_preview_lines=0;
}

void sh_nav_bake_build_begin(void)
{
    sh_nav_bake_refresh_live();
    InterlockedExchange(&g_building, 1);
}

void sh_nav_bake_build_end(void)
{
    InterlockedExchange(&g_building, 0);
}

void sh_nav_bake_set_snapshot(sh_nav_bake_snapshot snapshot, void *ctx)
{
    AcquireSRWLockExclusive(&g_bake_lock);
    g_snapshot = snapshot; g_snapshot_ctx = ctx;
    ReleaseSRWLockExclusive(&g_bake_lock);
}

void sh_nav_bake_set_live_editor(sh_nav_bake_entity_count count,
                                 sh_navr_entity_valid valid,
                                 sh_navr_entity_json get_json,
                                 void *ctx)
{
    AcquireSRWLockExclusive(&g_bake_lock);
    g_live_count = count;
    g_live_valid = valid;
    g_live_json = get_json;
    g_live_ctx = ctx;
    ReleaseSRWLockExclusive(&g_bake_lock);
}

/* Re-read the markers from the live entities, then re-plan. Called with the
 * lock held, immediately before a bake.
 *
 * Failure is not an error: -1 leaves the map exactly as loaded, which is what a
 * downloaded map wants anyway. The only thing lost is a mark made this session. */
static void bake_refresh_live_locked(void)
{
    int n, marked, before, after;
    char line[192];

    if (!g_live_count || !g_live_json || !g_have_map) return;
    n = g_live_count(g_live_ctx);
    g_live_scanned = n;
    g_live_marked = 0;
    g_live_refused = 0;
    if (n <= 0) return;
    before = g_module_count;
    marked = sh_nav_regions_refresh_live(&g_map, n, g_live_valid, g_live_json, g_live_ctx);
    if (marked < 0) { g_live_refused = 1; return; }   /* map untouched; keep what the load gave us */
    g_live_marked = marked;
    bake_plan_locked();
    after = g_module_count;

    /* A live read that TAKES AWAY what the map load supplied is the one outcome
     * of this function that can silently turn the whole feature off, so it says
     * so. Reading the live surface at the wrong moment did exactly that on every
     * Play until 2026-09-06, and it was invisible: the bake simply never ran. */
    if (after < before) {
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "NAV: the live editor read dropped %d module(s) -- scanned %d entity id(s), "
                    "found %d marked volume(s)", before - after, n, marked);
        backend_log(line);
    }
}

/* How many times the engine has asked for each module's navigation this map.
 * Its own table, not g_modules: it must count modules nobody marked, which is
 * what makes an unmarked map a usable probe. */
typedef struct bake_open_census {
    char module[SH_NAVR_MODULE_CAP];
    unsigned opens;
} bake_open_census;

static bake_open_census g_census[BAKE_MAX_MODULES];
static int              g_census_count;

static void bake_census(const char *module)
{
    int i;
    AcquireSRWLockExclusive(&g_bake_lock);
    for (i = 0; i < g_census_count; i++) {
        if (strcmp(g_census[i].module, module) == 0) {
            g_census[i].opens++;
            ReleaseSRWLockExclusive(&g_bake_lock);
            return;
        }
    }
    if (g_census_count < BAKE_MAX_MODULES) {
        strncpy_s(g_census[g_census_count].module,
                  sizeof g_census[g_census_count].module, module, _TRUNCATE);
        g_census[g_census_count].opens = 1;
        g_census_count++;
    }
    ReleaseSRWLockExclusive(&g_bake_lock);
}

static void bake_reason(bake_module *m, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(m->reason, sizeof m->reason, _TRUNCATE, fmt, ap);
    va_end(ap);
}

static int bake_enabled(void)
{
    int on = 0;
    unsigned flags = 0;
    if (!sh_config_get_bool("navmesh.enabled", &on, &flags)) return 0;
    return on;
}

/* ==================================================================== */
/* the resource-name grammar                                             */
/* ==================================================================== */

/* The two names one module's navigation answers to, mirroring navmesh.c:
 *
 *     maps/modules/<category>/<module>/<module>.aas_<class>
 *     generated/maps/modules/<category>/<module>/<module>.baas_<class>
 *
 * The cooked spelling prefixes 'b' to the WHOLE extension, and the engine asks
 * for it FIRST -- which is why getting it wrong fails silently rather than
 * loudly, and why navmesh_test.c pins both spellings with literal strings. The
 * same trap applies here, so this parser is tested the same way. */
static int bake_parse_name(const char *name, char *module, size_t module_cap,
                           char *cls, size_t cls_cap)
{
    static const char SRC[] = "maps/modules/";
    static const char COOK[] = "generated/maps/modules/";
    const char *p, *dot, *ext, *slash;
    size_t n;

    if (!name || !module || !cls || module_cap == 0 || cls_cap == 0) return 0;
    module[0] = 0;
    cls[0] = 0;

    if (strncmp(name, COOK, sizeof COOK - 1) == 0) {
        p = name + sizeof COOK - 1;
        ext = ".baas_";
    } else if (strncmp(name, SRC, sizeof SRC - 1) == 0) {
        p = name + sizeof SRC - 1;
        ext = ".aas_";
    } else {
        return 0;
    }

    /* p is "<category>/<module>/<module><ext><class>". The category may not
     * contain a dot, and neither may a module, so the FIRST dot from the right
     * that starts our extension is the split. */
    dot = strstr(p, ext);
    if (!dot) return 0;
    /* the leaf directly before the extension must be preceded by a slash */
    slash = dot;
    while (slash > p && slash[-1] != '/') slash--;
    if (slash == p) return 0;

    n = (size_t)(slash - 1 - p);        /* "<category>/<module>" */
    if (n == 0 || n >= module_cap) return 0;
    memcpy(module, p, n);
    module[n] = 0;

    /* the leaf must equal the module's own last segment */
    {
        const char *leaf = strrchr(module, '/');
        size_t leaf_len;
        leaf = leaf ? leaf + 1 : module;
        leaf_len = strlen(leaf);
        if ((size_t)(dot - slash) != leaf_len) return 0;
        if (memcmp(slash, leaf, leaf_len) != 0) return 0;
    }

    p = dot + strlen(ext);
    n = strlen(p);
    if (n == 0 || n >= cls_cap) return 0;
    memcpy(cls, p, n + 1);
    return 1;
}

/* ==================================================================== */
/* per-map state                                                         */
/* ==================================================================== */

static bake_module *bake_find(const char *module)
{
    int i;
    for (i = 0; i < g_module_count; i++)
        if (strcmp(g_modules[i].module, module) == 0) return &g_modules[i];
    return NULL;
}

/* Plan each exact placed instance independently, including repeated modules. */
static void bake_plan_locked(void)
{
    int i;
    g_module_count = 0;
    for (i = 0; i < g_map.region_count; i++) {
        const sh_nav_region *r = &g_map.regions[i];
        const sh_nav_instance *inst;
        bake_module *m;
        if (r->instance < 0 || r->instance >= g_map.instance_count) continue;
        inst = &g_map.instances[r->instance];
        if (inst->module[0] == 0) continue;

        m = NULL;
        {
            int slot;
            for(slot=0;slot<g_module_count;slot++)
                if(g_modules[slot].instance==r->instance){m=&g_modules[slot];break;}
        }
        if (!m) {
            if (g_module_count >= BAKE_MAX_MODULES) continue;
            m = &g_modules[g_module_count++];
            memset(m, 0, sizeof *m);
            strncpy_s(m->module, sizeof m->module, inst->module, _TRUNCATE);
            m->instance = r->instance;
            m->ok = 1;
        }
        m->regions++;
    }
    for (i = 0; i < g_module_count; i++) {
        if (g_modules[i].ok && g_modules[i].regions == 0) {
            g_modules[i].ok = 0;
            bake_reason(&g_modules[i], "no usable marked volume");
        }
    }
}

/* Re-read the markers from the live editor and re-plan.
 *
 * MUST be called on DOOM's main thread while the EDITOR is live and quiescent --
 * that is the whole point of it being a separate entry point rather than
 * something the bake does for itself. It reads live entities through the engine's
 * own reflection serialize, which is only meaningful while the editor still owns
 * its map; the frontend's UI worker thread is explicitly the wrong place (issue
 * #61), and so is the middle of BuildAAS (issues #87 and #89).
 *
 * Cheap to call again: it is a scan, not a mutation, and it is idempotent. */
void sh_nav_bake_refresh_live(void)
{
    if (InterlockedCompareExchange(&g_building, 0, 0)) return;
    AcquireSRWLockExclusive(&g_bake_lock);
    __try {
    if (g_snapshot) {
        char *json = NULL; size_t len = 0;
        sh_nav_map *candidate = (sh_nav_map *)malloc(sizeof *candidate);
        int ok = candidate && g_snapshot(&json, &len, g_snapshot_ctx) &&
                 sh_nav_regions_read(json, len, candidate) && !candidate->truncated &&
                 !candidate->invalid_geometry;
        if (ok) {
            int changed = !g_have_map || g_live_refused || memcmp(&g_map, candidate, sizeof g_map);
            g_map = *candidate;
            g_have_map = 1;
            g_live_marked = g_map.region_count;
            g_live_scanned = g_map.region_count;
            g_live_refused = 0;
            if (changed) { g_geometry_revision++; bake_preview_clear(); bake_plan_locked(); }
        } else {
            /* An unreadable current snapshot must never reuse moved or deleted
             * geometry from an earlier edit generation. */
            g_live_refused = 1;
            g_module_count = 0;
            bake_preview_clear();
        }
        free(json); free(candidate);
    } else if (g_have_map) bake_refresh_live_locked();
    } __finally { ReleaseSRWLockExclusive(&g_bake_lock); }
}

/* Private resource names are scoped by both edit revision and exact instance.
 * The engine uses them only for a temporary load, then releases the resource. */
int sh_nav_bake_instance_name(int instance,const char *name,char *out,size_t capacity)
{
    char module[SH_NAVR_MODULE_CAP],cls[32];int i,ok=0;
    if(!g_instance_serving||!name||!out||!capacity||!bake_enabled()||
       InterlockedCompareExchange(&g_faulted,0,0))return 0;
    if(!bake_parse_name(name,module,sizeof module,cls,sizeof cls))return 0;
    AcquireSRWLockShared(&g_bake_lock);
    if(g_have_map&&!g_live_refused&&instance>=0&&instance<g_map.instance_count&&
       !strcmp(module,g_map.instances[instance].module)) {
        for(i=0;i<g_module_count;i++)if(g_modules[i].instance==instance&&g_modules[i].ok) {
            int n=_snprintf_s(out,capacity,_TRUNCATE,"maps/smpnav/%lu/%d/%s",
                g_geometry_revision,instance,name+5);
            ok=n>0;break;
        }
    }
    ReleaseSRWLockShared(&g_bake_lock);return ok;
}

void sh_nav_bake_set_map(const char *json, size_t len)
{
    int ok;
    int i, total = 0, refused = 0;
    char line[256];

    AcquireSRWLockExclusive(&g_bake_lock);
    memset(&g_map, 0, sizeof g_map);
    bake_preview_clear();g_geometry_revision++;
    memset(g_modules, 0, sizeof g_modules);
    g_module_count = 0;
    g_have_map = 0;
    memset(g_census, 0, sizeof g_census);
    g_census_count = 0;
    g_live_scanned = -1;
    g_live_marked = 0;
    g_live_refused = 0;

    if (bake_enabled() && json && len) {
        __try {
            ok = sh_nav_regions_read(json, len, &g_map);
            if (ok && !g_map.truncated && !g_map.invalid_geometry) {
                bake_plan_locked();
                g_have_map = 1;
            } else g_live_refused = 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            memset(&g_map, 0, sizeof g_map);
            g_module_count = 0;
            g_have_map = 0;
            InterlockedExchange(&g_faulted, 1);
        }
    }
    for (i = 0; i < g_module_count; i++) {
        total += g_modules[i].regions;
        if (!g_modules[i].ok) refused++;
    }
    ReleaseSRWLockExclusive(&g_bake_lock);

    if (g_module_count == 0) return;    /* silence is right for the common map */

    _snprintf_s(line, sizeof line, _TRUNCATE,
                "NAV: this map marks %d volume(s) for AI navigation across %d module(s)"
                "%s%s", total, g_module_count,
                refused ? "; " : "", refused ? "some were refused, see sh_navmesh" : "");
    backend_log(line);
}

/* ==================================================================== */
/* the bake                                                              */
/* ==================================================================== */

static int bake_collect_platforms(const bake_module *m, sh_aug_platform *out, int cap)
{
    int i, n = 0;
    for (i = 0; i < g_map.region_count && n < cap; i++) {
        const sh_nav_region *r = &g_map.regions[i];
        if (r->instance != m->instance) continue;
        /* A volume demons fall straight through is not a floor, whatever the
         * author ticked -- navigating onto it would be a lie the router acts on. */
        if (!r->block_demons) continue;
        memset(&out[n], 0, sizeof out[n]);
        memcpy(out[n].c, r->c, sizeof r->c);
        memcpy(out[n].n, r->n, sizeof out[n].n);
        out[n].face = r->face;
        out[n].depth = r->depth;
        _snprintf_s(out[n].name, sizeof out[n].name, _TRUNCATE, "volume %u", r->entity);
        n++;
    }
    for(i=0;i<g_map.obstacle_count&&n<cap;i++) {
        const sh_nav_region *r=&g_map.obstacles[i];
        if(r->instance!=m->instance||!r->block_demons)continue;
        memset(&out[n],0,sizeof out[n]);
        memcpy(out[n].c,r->c,sizeof r->c);memcpy(out[n].n,r->n,sizeof r->n);
        out[n].face=r->face;out[n].depth=r->depth;out[n].obstacle_only=1;
        _snprintf_s(out[n].name,sizeof out[n].name,_TRUNCATE,"obstacle %u",r->entity);n++;
    }
    return n;
}

static int bake_one(const char *name, const bake_module *m, sh_nav_bake_reader read_shipped,
                    unsigned char **out_bytes, size_t *out_len, char *why, size_t why_cap,
                    unsigned *first_area)
{
    unsigned char *shipped = NULL, *baked = NULL;
    size_t shipped_len = 0, baked_len = 0;
    sh_aas *model = NULL;
    sh_aug_platform plats[SH_AUG_MAX_PLATFORMS];
    sh_aug_opts opts;
    sh_aug_report rep;
    char err[192];
    int n, rc = 0;

    n = bake_collect_platforms(m, plats, SH_AUG_MAX_PLATFORMS);
    if (n == 0) {
        _snprintf_s(why, why_cap, _TRUNCATE,
                    "no marked volume also blocks demons, so none is a floor");
        return 0;
    }

    /* The animation table, from the player's own install. Loaded lazily on the
     * first bake rather than at startup: it is only needed once a map actually
     * marks something, and by then the resource provider is certainly up. It
     * caches itself, so this is one open per session. Failure is not fatal --
     * without a table nothing can climb and every platform out of step range is
     * an island, which is exactly the previous build's behaviour. */
    sh_trav_load(read_shipped);

    /* The bytes the engine was about to load. Without these there is nothing to
     * add to: we do not author a navmesh, we extend the shipped one. */
    shipped = read_shipped ? read_shipped(name, &shipped_len) : NULL;
    if (!shipped) {
        _snprintf_s(why, why_cap, _TRUNCATE,
                    "the module's own navigation could not be read back");
        return 0;
    }

    err[0] = 0;
    model = sh_aas_parse(shipped, shipped_len, err, sizeof err);
    HeapFree(GetProcessHeap(), 0, shipped);
    if (!model) {
        _snprintf_s(why, why_cap, _TRUNCATE, "the module's navigation did not parse: %s", err);
        return 0;
    }

    memset(&opts, 0, sizeof opts);
    if(first_area)*first_area=sh_aas_count(model,SH_AAS_L_AREAS);
    opts.fall = SH_AUG_FALL_AUTO;
    opts.inset = 1;
    opts.traversal = 0;

    if (sh_aas_augment(model, plats, n, &opts, &rep)) {
        baked = sh_aas_write(model, &baked_len);
    }
    sh_aas_free(model);
    if (!baked) {
        _snprintf_s(why, why_cap, _TRUNCATE, "%s",
                    rep.links_truncated ? "the traversal capacity was exceeded; the whole bake was refused" :
                    rep.pieces_truncated ? "the geometry capacity was exceeded; the whole bake was refused" :
                    rep.depth_exceeded ? "the navigation tree depth was exceeded; the whole bake was refused" :
                    "the bake did not produce a payload");
        return 0;
    }

    /* The same gate the shard path applies. A payload we generated is not
     * automatically safe: the engine's loader is what dies on a malformed one,
     * and a bug in our own augmenter is exactly as fatal as a hostile map. */
    err[0] = 0;
    if (!sh_navmesh_validate_aas(baked, baked_len, err, sizeof err)) {
        HeapFree(GetProcessHeap(), 0, baked);
        _snprintf_s(why, why_cap, _TRUNCATE, "the bake did not pass validation: %s", err);
        return 0;
    }
    if (rep.depth_exceeded) {
        HeapFree(GetProcessHeap(), 0, baked);
        _snprintf_s(why, why_cap, _TRUNCATE,
                    "too many separate platforms for one module's navigation tree");
        return 0;
    }

    *out_bytes = baked;
    *out_len = baked_len;
    rc = 1;
    {
        /* Say what each platform actually got. "Islands" is the number an author
         * most wants to see: a platform nothing can climb onto is legal and
         * sometimes wanted, but it is a different thing from one they expected
         * demons to reach, and only this line distinguishes them. */
        int islands = 0, climbs = 0, leaps = 0, chained = 0, tipped = 0, i;
        int cut = 0, lastcut = -1;
        for (i = 0; i < rep.platform_count; i++) {
            /* Counted over every entry, emitted or not: a volume cut into
             * pieces that were then all refused is exactly the case an author
             * needs told about. Entries from one volume are contiguous, so the
             * last-source check is enough to count volumes rather than pieces. */
            if (rep.platforms[i].pieces != 1 && rep.platforms[i].source != lastcut) {
                lastcut = rep.platforms[i].source;
                cut++;
            }
            if (!rep.platforms[i].emitted) continue;
            if (rep.platforms[i].island) islands++;
            climbs += rep.platforms[i].climbs;
            leaps  += rep.platforms[i].leaps;
            /* More than one neighbour means it reaches something besides the
             * module floor -- which is the question an author actually has after
             * standing two volumes next to each other. */
            if (rep.platforms[i].neighbours > 1) chained++;
            if (rep.platforms[i].side_face) tipped++;
        }
        _snprintf_s(why, why_cap, _TRUNCATE,
                    "%d platform(s), areas %u->%u, links %u->%u "
                    "(%d climb%s, %d leap%s, %d chained), tree depth %u->%u%s%s%s",
                    n, rep.areas_before, rep.areas_after,
                    rep.reach_before, rep.reach_after,
                    climbs, climbs == 1 ? "" : "s",
                    leaps, leaps == 1 ? "" : "s",
                    chained,
                    rep.depth_before, rep.depth_after,
                    islands ? "; islands: " : "",
                    islands ? (rep.climbs_declined
                               ? "this module already carries its own climbs, so "
                                 "none were added"
                               : (sh_trav_ready()
                                  ? "nothing can climb that high"
                                  : "no traversal table, so nothing climbs")) : "",
                    tipped ? "; some volumes are walkable on a side face" : "");
        if (cut) {
            size_t at = strlen(why);
            _snprintf_s(why + at, why_cap - at, _TRUNCATE,
                        "; %d volume(s) cut around the solids standing in them", cut);
        }
        /* Kept SHORT and last: the line has a fixed budget, and a note that
         * pushes the cut count off the end costs the author the more important
         * fact. */
        if (rep.dead_gaps) {
            size_t at = strlen(why);
            _snprintf_s(why + at, why_cap - at, _TRUNCATE,
                        "; %d pair(s) too far to step and too close to jump",
                        rep.dead_gaps);
        }
    }
    return rc;
}

static void bake_world_point(const sh_nav_instance *in,const float p[3],float out[3])
{
    static const int xy[8][4]={{1,0,0,1},{0,-1,1,0},{-1,0,0,-1},{0,1,-1,0},
                             {-1,0,0,1},{0,1,1,0},{1,0,0,-1},{0,-1,-1,0}};
    int o=in->orientation;
    if(o<0||o>7)o=0;
    out[0]=in->origin[0]+xy[o][0]*p[0]+xy[o][1]*p[1];
    out[1]=in->origin[1]+xy[o][2]*p[0]+xy[o][3]*p[1];
    out[2]=in->origin[2]+p[2]+2.0f;
}

static void bake_preview_line(const sh_nav_instance *in,const float a[3],const float b[3],
                               sh_nav_preview_line line,void *ctx)
{
    float start[3],end[3];
    if(g_preview_lines>=8192)return;
    bake_world_point(in,a,start);bake_world_point(in,b,end);
    line(start,end,ctx);g_preview_lines++;
}

static void bake_preview_polygon(const sh_nav_instance *in,const float p[][3],int n,
                                  sh_nav_preview_line line,void *ctx)
{
    int i,axis;
    for(i=0;i<n;i++)bake_preview_line(in,p[i],p[(i+1)%n],line,ctx);
    /* Grid segments are clipped to the actual convex baked cell. Interpolating
     * each edge supplies the correct Z on sloping surfaces. */
    for(axis=0;axis<2;axis++) {
        float lo=p[0][axis],hi=lo,spacing,v;
        for(i=1;i<n;i++){if(p[i][axis]<lo)lo=p[i][axis];if(p[i][axis]>hi)hi=p[i][axis];}
        spacing=(float)fmax(64.0,ceil((hi-lo)/32.0/64.0)*64.0);
        for(v=(float)(ceil(lo/spacing)*spacing);v<hi;v+=spacing) {
            float hit[2][3];int count=0,k;
            for(i=0;i<n&&count<2;i++) {
                int j=(i+1)%n;float a=p[i][axis],b=p[j][axis],t;
                if(!((a<=v&&b>v)||(b<=v&&a>v)))continue;
                t=(v-a)/(b-a);
                for(k=0;k<3;k++)hit[count][k]=p[i][k]+t*(p[j][k]-p[i][k]);
                count++;
            }
            if(count==2)bake_preview_line(in,hit[0],hit[1],line,ctx);
        }
    }
}

void sh_nav_bake_preview(sh_nav_bake_reader read_shipped,sh_nav_preview_line line,void *ctx)
{
    int i;
    if(!line||!read_shipped||!bake_enabled()||InterlockedCompareExchange(&g_building,0,0))return;
    AcquireSRWLockExclusive(&g_bake_lock);
    __try {
        if(!g_have_map||g_live_refused)__leave;
        if(g_preview_revision!=g_geometry_revision) {
            bake_preview_clear();
            for(i=0;i<g_module_count;i++)if(g_modules[i].ok) {
                char name[320];const char *base=strrchr(g_modules[i].module,'/');
                if(!base)continue;
                _snprintf_s(name,sizeof name,_TRUNCATE,"generated/maps/modules/%s/%s.baas_monster48",
                            g_modules[i].module,base+1);
                bake_one(name,&g_modules[i],read_shipped,&g_preview[i].bytes,&g_preview[i].length,
                    g_modules[i].reason,sizeof g_modules[i].reason,&g_preview[i].first_area);
            }
            g_preview_revision=g_geometry_revision;
        }
        g_preview_lines=0;
        for(i=0;i<g_module_count;i++)if(g_preview[i].bytes) {
            char err[128];unsigned a;
            sh_aas *model=sh_aas_parse(g_preview[i].bytes,g_preview[i].length,err,sizeof err);
            if(!model)continue;
            for(a=g_preview[i].first_area;a<sh_aas_count(model,SH_AAS_L_AREAS);a++) {
                const unsigned char *ar=sh_aas_rec_const(model,SH_AAS_L_AREAS,a);
                float local_points[SH_AUG_MAX_CORNERS][3],(*points)[3]=local_points;
                int e,k,n;unsigned first;
                if(!ar)continue;
                n=sh_aas_get_u16(ar,6);first=sh_aas_get_u32(ar,8);
                if(n<3)continue;
                /* Seam stitching adds collinear vertices beyond the convex
                 * geometry builder's corner cap. They still form one floor. */
                if(n>SH_AUG_MAX_CORNERS) {
                    points=(float(*)[3])HeapAlloc(GetProcessHeap(),0,(size_t)n*sizeof *points);
                    if(!points)continue;
                }
                for(e=0;e<n;e++) {
                    const unsigned char *index=sh_aas_rec_const(model,SH_AAS_L_EDGEINDEX,first+e),*edge,*vertex;
                    int ei,vi;if(!index)break;
                    ei=sh_aas_get_i32(index,0);
                    edge=sh_aas_rec_const(model,SH_AAS_L_EDGES,(unsigned)abs(ei));if(!edge)break;
                    vi=sh_aas_get_i32(edge,ei<0?4:0);
                    vertex=sh_aas_rec_const(model,SH_AAS_L_VERTICES,(unsigned)vi);if(!vertex)break;
                    for(k=0;k<3;k++)points[e][k]=sh_aas_get_f32(vertex,k*4);
                }
                if(e==n)bake_preview_polygon(&g_map.instances[g_modules[i].instance],points,n,line,ctx);
                if(points!=local_points)HeapFree(GetProcessHeap(),0,points);
            }
            sh_aas_free(model);
        }
    } __finally { ReleaseSRWLockExclusive(&g_bake_lock); }
}

int sh_nav_bake_open(const char *name, sh_nav_bake_reader read_shipped,
                     unsigned char **out_bytes, size_t *out_len)
{
    char module[SH_NAVR_MODULE_CAP], cls[32];
    char why[BAKE_REASON_CAP];
    bake_module *m;
    int hit = 0, i, known = 0, instance=-1;
    unsigned long revision=0;
    char canonical[384];

    if (!name || !out_bytes || !out_len) return 0;
    *out_bytes = NULL;
    *out_len = 0;
    {
        const char *p=name;int cooked=0;char *end;
        if(!strncmp(p,"generated/",10)){p+=10;cooked=1;}
        if(!strncmp(p,"maps/smpnav/",12)) {
            p+=12;
            revision=strtoul(p,&end,10);
            if(end==p||*end!='/')return 0;
            p=end+1;instance=(int)strtol(p,&end,10);
            if(end==p||*end!='/'||instance<0||instance>=SH_NAVR_MAX_INSTANCES)return 0;
            if(_snprintf_s(canonical,sizeof canonical,_TRUNCATE,"%smaps/%s",
                cooked?"generated/":"",end+1)<0)return 0;
            name=canonical;
        } else if(g_instance_serving)return 0;
    }
    if (!bake_parse_name(name, module, sizeof module, cls, sizeof cls)) return 0;
    for (i = 0; i < NAV_CLASS_COUNT; i++) if (strcmp(cls, NAV_CLASSES[i]) == 0) known = 1;
    if (!known) return 0;
    if (InterlockedCompareExchange(&g_faulted, 0, 0) != 0) {
        if(instance>=0 && read_shipped) *out_bytes=read_shipped(name,out_len);
        return *out_bytes!=NULL;
    }

    /* Census FIRST, for every module the engine asks navigation for -- marked or
     * not. How many times one module's navigation is opened is the measurement
     * the one-copy rule turns on: BuildAAS builds the name inside its
     * per-instance loop, so N opens of one name across a map that places that
     * module N times means each instance can be served its own navigation, and
     * three opens for twelve instances means it cannot. Counted here rather than
     * at the bake because an unmarked module is the cheapest way to ask. */
    bake_census(module);

    AcquireSRWLockExclusive(&g_bake_lock);
    if (!g_have_map) {
        ReleaseSRWLockExclusive(&g_bake_lock);
        if(instance>=0 && read_shipped) *out_bytes=read_shipped(name,out_len);
        return *out_bytes!=NULL;
    }
    /* NOTHING HERE MAY TOUCH THE ENGINE. This runs inside the engine's own AAS
     * loader -- idDeclSnapMap::BuildAAS (0x4EBFB0) -> idAAS2File::Load ->
     * idAAS2File::LoadBinary -> the resource-provider hook -> here -- and by then
     * SnapMapEditToSnapBuild has already begun turning the edit map into the build
     * map. Reading the live entities from this point called EntityClone on entities
     * whose defsub was still NULL and raised thousands of access violations inside
     * the loader, which the fault shield escalated to idCommon::Error(6) and which
     * killed the process (issues #87 and #89).
     *
     * The refresh now happens on the editor side, before the build starts, through
     * sh_nav_bake_refresh_live(). An open consumes the plan and nothing else. */
    m = NULL;
    if(instance>=0) {
        if(revision==g_geometry_revision)
            for(i=0;i<g_module_count;i++)if(g_modules[i].instance==instance&&
                !strcmp(g_modules[i].module,module)){m=&g_modules[i];break;}
    } else {
        /* Legacy callers may serve a name only when it identifies one instance. */
        if(sh_nav_regions_nth_instance(&g_map,module,1)<0)m=bake_find(module);
    }
    if (m && m->ok) {
        why[0] = 0;
        __try {
            int slot=(int)(m-g_modules);
            if(strcmp(cls,"monster48")==0 && g_preview_revision==g_geometry_revision &&
               g_preview[slot].bytes) {
                *out_bytes=(unsigned char*)HeapAlloc(GetProcessHeap(),0,g_preview[slot].length);
                if(*out_bytes){memcpy(*out_bytes,g_preview[slot].bytes,g_preview[slot].length);
                    *out_len=g_preview[slot].length;hit=1;}
                strncpy_s(why,sizeof why,m->reason,_TRUNCATE);
            } else hit = bake_one(name, m, read_shipped, out_bytes, out_len, why, sizeof why,NULL);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            /* One fault disables the feature for the session. A bake that can
             * fault once can fault again, and the engine is holding the map. */
            InterlockedExchange(&g_faulted, 1);
            hit = 0;
            _snprintf_s(why, sizeof why, _TRUNCATE, "the bake faulted; navigation is off for this session");
        }
        if (!hit) {
            m->ok = 0;
            strncpy_s(m->reason, sizeof m->reason, why, _TRUNCATE);
        } else {
            strncpy_s(m->reason, sizeof m->reason, why, _TRUNCATE);
            InterlockedIncrement(&g_bakes);
        }
    }
    ReleaseSRWLockExclusive(&g_bake_lock);

    if (hit) {
        char line[SH_SMNAV_RESNAME_CAP + 160];
        _snprintf_s(line, sizeof line, _TRUNCATE, "NAV: baked '%s' -- %s", name, why);
        backend_log(line);
    }
    if(!hit&&instance>=0&&read_shipped) {
        *out_bytes=read_shipped(name,out_len);
        hit=*out_bytes!=NULL;
    }
    return hit;
}

void sh_nav_bake_report(void (*out)(const char *fmt, ...))
{
    int i;
    if (!out) return;
    if (InterlockedCompareExchange(&g_faulted, 0, 0) != 0)
        out("navigation: baking is OFF for this session after a fault.\n");
    AcquireSRWLockShared(&g_bake_lock);
    if (g_module_count == 0 && !g_live_refused) {
        out("navigation: no volume in this map is marked for AI navigation.\n");
    } else {
        for (i = 0; i < g_module_count; i++) {
            const bake_module *m = &g_modules[i];
            out("  marked %s -- %d volume(s) in copy %d -- %s\n",
                m->module, m->regions, m->instance,
                m->ok ? (m->reason[0] ? m->reason : "ready") : m->reason);
        }
    }
    if (g_live_refused) {
        out("  current editor geometry could not be read; custom navigation is unavailable "
            "until a complete snapshot succeeds.\n");
    } else if (g_live_scanned < 0) {
        out("  using the loaded map; a complete editor snapshot is pending.\n");
    } else {
        out("  the current editor snapshot contains %d marked volume(s).\n",g_live_marked);
    }
    out("  editor geometry revision %lu; green preview: monster48, %d lines%s.\n",
        g_geometry_revision,g_preview_lines,g_preview_lines>=8192?" (display limit reached)":"");
    for (i = 0; i < g_census_count; i++)
        out("  the engine opened %s navigation %u time(s)\n",
            g_census[i].module, g_census[i].opens);
    ReleaseSRWLockShared(&g_bake_lock);
    out("  baked %lu payload(s) this session.\n",
        (unsigned long)InterlockedCompareExchange(&g_bakes, 0, 0));
}

#ifdef SH_NAV_BAKE_TESTING
int sh_nav_bake_test_parse_name(const char *name, char *module, size_t module_cap,
                                char *cls, size_t cls_cap)
{
    return bake_parse_name(name, module, module_cap, cls, cls_cap);
}

void sh_nav_bake_test_reset(void)
{
    AcquireSRWLockExclusive(&g_bake_lock);
    bake_preview_clear();
    g_instance_serving=0;
    memset(&g_map, 0, sizeof g_map);
    memset(g_modules, 0, sizeof g_modules);
    g_module_count = 0;
    g_have_map = 0;
    g_snapshot = NULL; g_snapshot_ctx = NULL;
    g_building = 0; g_geometry_revision = 0;
    ReleaseSRWLockExclusive(&g_bake_lock);
    InterlockedExchange(&g_bakes, 0);
    InterlockedExchange(&g_faulted, 0);
}

int sh_nav_bake_test_bake_count(void)
{
    return (int)InterlockedCompareExchange(&g_bakes, 0, 0);
}

void sh_nav_bake_test_copy_map(sh_nav_map *out)
{
    AcquireSRWLockShared(&g_bake_lock);
    *out = g_map;
    ReleaseSRWLockShared(&g_bake_lock);
}
#endif
