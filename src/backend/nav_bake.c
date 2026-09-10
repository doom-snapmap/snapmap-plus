/* Bake per-instance navigation from map data or a current editor snapshot. */
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

/* Room for counts and all diagnostic notes without truncating common reports.

 */
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
/* Last live-read diagnostics distinguish a missing snapshot from an empty
 * one.
 */
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

/* Legacy per-entity refresh, called with g_bake_lock held. A refusal
 * preserves the previously attributed map; complete snapshots use a separate
 * path.
 */
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

    /* Report lost modules so a failed or mistimed live read is visible. */
    if (after < before) {
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "NAV: the live editor read dropped %d module(s) -- scanned %d entity id(s), "
                    "found %d marked volume(s)", before - after, n, marked);
        backend_log(line);
    }
}

/* Count resource opens for all modules, including unmarked ones. */
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

/* Accepted resource names:
 *   maps/modules/<category>/<module>/<module>.aas_<class>
 *   generated/maps/modules/<category>/<module>/<module>.baas_<class>
 * The engine tries the cooked .baas_ spelling first.
 */
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

    /* Split at the extension in <category>/<module>/<module><ext><class>. */
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

/* Refresh on DOOM's main thread while the editor still owns its map, before
 * BuildAAS. Engine reflection is unsafe on the frontend worker or during map
 * conversion. Complete snapshots also refresh ownership and newly created
 * entities.
 */
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

    if (g_module_count == 0) return;    /* No report for an unmarked map. */

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
        /* Only geometry that blocks demons can support a walkable surface. */
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

    /* Load traversal animations lazily through the active provider;
     * successful reads are cached for the session. Without a table, no climbs
     * are emitted.
     */
    sh_trav_load(read_shipped);

    /* Augmentation requires the shipped payload as its base. */
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
                    rep.reach_limit_exceeded ? "required routes exceed the native 256 outgoing links per area; the whole bake was refused" :
                    rep.links_truncated ? "the traversal capacity was exceeded; the whole bake was refused" :
                    rep.pieces_truncated ? "the geometry capacity was exceeded; the whole bake was refused" :
                    rep.depth_exceeded ? "the navigation tree depth was exceeded; the whole bake was refused" :
                    "the bake did not produce a payload");
        return 0;
    }

    /* Generated output must pass the same structural gate as embedded payloads. */
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
        /* Report islands separately from surfaces with incoming routes. */
        int islands = 0, climbs = 0, leaps = 0, chained = 0, tipped = 0, i;
        int cut = 0, lastcut = -1;
        for (i = 0; i < rep.platform_count; i++) {
            /* Count source volumes even if all pieces were refused. Pieces
             * from one source are contiguous.
             */
            if (rep.platforms[i].pieces != 1 && rep.platforms[i].source != lastcut) {
                lastcut = rep.platforms[i].source;
                cut++;
            }
            if (!rep.platforms[i].emitted) continue;
            if (rep.platforms[i].island) islands++;
            climbs += rep.platforms[i].climbs;
            leaps  += rep.platforms[i].leaps;
            /* Multiple neighbours indicate routes beyond the module floor. */
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
        if (rep.anchors_reduced) {
            size_t at = strlen(why);
            _snprintf_s(why + at, why_cap - at, _TRUNCATE,
                        "; %d alternative link samples removed to fit native routing",
                        rep.anchors_reduced);
        }
        if (cut) {
            size_t at = strlen(why);
            _snprintf_s(why + at, why_cap - at, _TRUNCATE,
                        "; %d volume(s) cut around the solids standing in them", cut);
        }
        /* Keep optional notes last so truncation preserves the cut-volume count. */
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

    /* Count every module open before filtering, including unmarked modules. */
    bake_census(module);

    AcquireSRWLockExclusive(&g_bake_lock);
    if (!g_have_map) {
        ReleaseSRWLockExclusive(&g_bake_lock);
        if(instance>=0 && read_shipped) *out_bytes=read_shipped(name,out_len);
        return *out_bytes!=NULL;
    }
    /* Do not inspect live entities here. This runs inside BuildAAS after
     * editor-to-build conversion has started; entities may have null defsub
     * fields. Consume the plan captured by sh_nav_bake_refresh_live before
     * conversion.
     */
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
            /* Disable baking after a fault to avoid repeating it during map load. */
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
