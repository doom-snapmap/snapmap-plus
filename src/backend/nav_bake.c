/* Bake per-instance navigation from map data or a current editor snapshot.
 *
 * Every refusal is as narrow as the engine allows: one unreadable box drops
 * that box (nav_regions), one module copy that cannot be baked keeps its
 * shipped navigation, and one demon class that cannot be baked leaves the
 * other two classes of that copy alone. Each outcome carries a full reason for
 * the log and sh_navmesh and a short one for the toast and View menu. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <math.h>

#include "nav_bake.h"
#include "perf.h"
#include "nav_regions.h"
#include "nav_entitydef.h"
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
#define BAKE_BRIEF_CAP  80
/* Floor climbs per box face, for sh_navmesh and the log. */
#define BAKE_FACES_CAP  768

/* What one demon class of one module copy got, and at which geometry
 * revision. A refusal recorded at the current revision is not baked again:
 * the same inputs would refuse the same way. */
typedef struct bake_class {
    int           state;                    /* SH_NAV_CLASS_* */
    unsigned long revision;
    int           logged;                   /* the skip was reported at this revision */
    char          reason[BAKE_REASON_CAP];  /* full sentence, for the log and sh_navmesh */
    char          brief[BAKE_BRIEF_CAP];    /* short, for the toast and View menu */
    char          faces[BAKE_FACES_CAP];    /* floor climbs per box face */
    int           faces_climbed, faces_total;
} bake_class;

typedef struct bake_module {
    char module[SH_NAVR_MODULE_CAP];    /* "category/module" */
    int  instance;                      /* the one instance carrying regions */
    int  regions;
    int  ok;                            /* has something to bake */
    int  not_floor;                     /* marked boxes that do not block demons */
    char reason[BAKE_REASON_CAP];
    bake_class cls[3];
} bake_module;

static SRWLOCK      g_bake_lock = SRWLOCK_INIT;
static sh_nav_map   g_map;
/* One entry per marked module copy, grown with the map. g_preview is parallel
 * and always has the same capacity. */
static bake_module *g_modules;
static int          g_module_count, g_module_cap;
static int          g_have_map;
static volatile LONG g_bakes;
static volatile LONG g_faulted;
/* Last live-read diagnostics distinguish a missing snapshot from an empty
 * one.
 */
static int g_live_scanned = -1;   /* entity ids offered, -1 = never ran */
static int g_live_marked;         /* marked volumes it attributed */
static int g_live_refused;        /* the read declined to commit */
static const char *g_read_reason = "";  /* why, when it declined */

static void bake_plan_locked(void);
static void bake_cache_clear(void);
static void bake_cache_begin_pass(void);

/* The live-editor surface, registered once at startup. */
static sh_nav_bake_entity_count g_live_count;
static sh_navr_entity_valid     g_live_valid;
static sh_navr_entity_json      g_live_json;
static sh_navr_entity_transform g_live_transform;
static void                    *g_live_ctx;
static sh_nav_bake_snapshot g_snapshot;
static void *g_snapshot_ctx;
static volatile LONG g_building;
static unsigned long g_geometry_revision;
/* A volume the bake could not place, in module-local space, so the editor can
 * draw it where it is instead of leaving the mapper to delete things one at a
 * time to find it. Capped because a refusal that marks forty is no more
 * informative than one that marks a couple of dozen. */
#define BAKE_MAX_REFUSED 24
typedef struct bake_refused { float c[4][3]; } bake_refused;

typedef struct bake_preview_cache {
    unsigned char *bytes;
    size_t length;
    unsigned first_area;
    bake_refused *refused;     /* process heap; bake_preview_clear frees it */
    int           refused_count;
    bake_refused *partial;     /* boxes that lost routes or faces: drawn amber */
    int           partial_count;
    int           instance;    /* the transform those shapes need */
    int           done;        /* the worker baked this slot at g_preview_revision */
    int           state;       /* SH_NAV_CLASS_* of that bake */
    char          source[SH_SMNAV_RESNAME_CAP]; /* exact gathered provider input */
    int           traversal;   /* this revision had a usable traversal table */
} bake_preview_cache;
static bake_preview_cache *g_preview;
static unsigned long g_preview_revision = ~0UL;
static int g_preview_lines;
static int g_preview_lines_last = -1;   /* -1 until the first pass runs */
static int g_ids_unusable;              /* last reported duplicate-id state */
static int g_instance_serving;

void sh_nav_bake_enable_instances(int enabled) { g_instance_serving=enabled; }

static int bake_class_index(const char *cls)
{
    int i;
    for (i = 0; i < NAV_CLASS_COUNT; i++) if (strcmp(cls, NAV_CLASSES[i]) == 0) return i;
    return -1;
}

/* Say what a refresh changed, so two readers that disagree can be told apart
 * by what each one produced rather than by the flicker they cause. Boxes are
 * matched by shape, not by position in the table or entity index, which two
 * reads of one map need not share. */
static void bake_log_change(const char *who, const sh_nav_map *a, const sh_nav_map *b)
{
    char line[256];
    int walls, x, y;
    if (a->instance_count != b->instance_count) {
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "NAV: %s changed the rooms -- %d->%d", who,
                    a->instance_count, b->instance_count);
        backend_log(line);
        return;
    }
    if (a->region_count != b->region_count || a->obstacle_count != b->obstacle_count) {
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "NAV: %s changed the count -- floors %d->%d, walls %d->%d",
                    who, a->region_count, b->region_count,
                    a->obstacle_count, b->obstacle_count);
        backend_log(line);
        return;
    }
    for (walls = 0; walls < 2; walls++) {
        const sh_nav_region *ra = walls ? a->obstacles : a->regions;
        const sh_nav_region *rb = walls ? b->obstacles : b->regions;
        if (!sh_nav_map_shape_difference(a, b, walls, &x, &y) || (x < 0 && y < 0)) continue;
        if (x >= 0 && y >= 0)
            _snprintf_s(line, sizeof line, _TRUNCATE,
                        "NAV: %s moved %s %u in copy %d->%d from %.1f %.1f %.1f "
                        "to %.1f %.1f %.1f",
                        who, walls ? "wall" : "floor", rb[y].entity,
                        ra[x].instance, rb[y].instance,
                        ra[x].c[0][0], ra[x].c[0][1], ra[x].c[0][2],
                        rb[y].c[0][0], rb[y].c[0][1], rb[y].c[0][2]);
        else
            _snprintf_s(line, sizeof line, _TRUNCATE,
                        "NAV: %s changed %s %u in copy %d", who, walls ? "wall" : "floor",
                        x >= 0 ? ra[x].entity : rb[y].entity,
                        x >= 0 ? ra[x].instance : rb[y].instance);
        backend_log(line);
        return;
    }
    _snprintf_s(line, sizeof line, _TRUNCATE,
                "NAV: %s changed something other than a box (a room's module, "
                "position or orientation)", who);
    backend_log(line);
}

static void bake_preview_clear(void)
{
    int i;
    for(i=0;i<g_module_cap;i++) {
        if(g_preview[i].bytes)HeapFree(GetProcessHeap(),0,g_preview[i].bytes);
        if(g_preview[i].refused)HeapFree(GetProcessHeap(),0,g_preview[i].refused);
        if(g_preview[i].partial)HeapFree(GetProcessHeap(),0,g_preview[i].partial);
    }
    if(g_preview)memset(g_preview,0,(size_t)g_module_cap*sizeof *g_preview);
    g_preview_revision=~0UL;g_preview_lines=0;g_preview_lines_last=-1;
}

/* Room for `want` module copies in both parallel tables. The preview cache is
 * cleared by every caller that replans, so growing it loses nothing. */
static int bake_reserve_modules(int want)
{
    bake_module *modules;
    bake_preview_cache *preview;
    int cap = g_module_cap ? g_module_cap : 16;
    if (want <= g_module_cap) return 1;
    while (cap < want) cap *= 2;
    modules = (bake_module *)realloc(g_modules, (size_t)cap * sizeof *modules);
    if (!modules) return 0;
    g_modules = modules;
    preview = (bake_preview_cache *)realloc(g_preview, (size_t)cap * sizeof *preview);
    if (!preview) return 0;
    memset(preview + g_module_cap, 0, (size_t)(cap - g_module_cap) * sizeof *preview);
    g_preview = preview;
    g_module_cap = cap;
    return 1;
}

static void bake_invalidate_sources_locked(void)
{
    g_geometry_revision++;
    bake_preview_clear();
    bake_cache_clear();   /* the shipped files and traversal table may differ now */
    sh_trav_invalidate();
    if (g_have_map && !g_live_refused) bake_plan_locked();
}

void sh_nav_bake_source_update_begin(void) { AcquireSRWLockExclusive(&g_bake_lock); }
void sh_nav_bake_source_update_end(int committed)
{
    if (committed) bake_invalidate_sources_locked();
    ReleaseSRWLockExclusive(&g_bake_lock);
}
void sh_nav_bake_invalidate_sources(void)
{
    sh_nav_bake_source_update_begin(); sh_nav_bake_source_update_end(1);
}

void sh_nav_bake_build_begin(void)
{
    /* Play is about to convert the map: bake every box that can be measured
     * rather than none. */
    sh_nav_bake_refresh_live_partial();
    AcquireSRWLockExclusive(&g_bake_lock);
    bake_cache_begin_pass();
    ReleaseSRWLockExclusive(&g_bake_lock);
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
                                 sh_navr_entity_transform get_transform,
                                 void *ctx)
{
    AcquireSRWLockExclusive(&g_bake_lock);
    g_live_count = count;
    g_live_valid = valid;
    g_live_json = get_json;
    g_live_transform = get_transform;
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
    if (marked < 0) {   /* map untouched; keep what the load gave us */
        g_live_refused = 1;
        g_read_reason = "the editor's boxes could not be read one by one";
        return;
    }
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

static bake_open_census *g_census;
static int               g_census_count, g_census_cap;

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
    if (g_census_count == g_census_cap && g_census_cap < SH_NAVR_MAX_INSTANCES) {
        int cap = g_census_cap ? g_census_cap * 2 : 32;
        bake_open_census *grown = (bake_open_census *)realloc(g_census, (size_t)cap * sizeof *grown);
        if (grown) { g_census = grown; g_census_cap = cap; }
    }
    if (g_census_count < g_census_cap) {
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
    int i, *slot_of = NULL;
    g_module_count = 0;
    if (g_map.instance_count > 0)
        slot_of = (int *)malloc((size_t)g_map.instance_count * sizeof *slot_of);
    if (!slot_of && g_map.instance_count > 0) return;
    for (i = 0; i < g_map.instance_count; i++) slot_of[i] = -1;
    for (i = 0; i < g_map.region_count; i++) {
        const sh_nav_region *r = &g_map.regions[i];
        const sh_nav_instance *inst;
        bake_module *m;
        if (r->instance < 0 || r->instance >= g_map.instance_count) continue;
        inst = &g_map.instances[r->instance];
        if (inst->module[0] == 0 || inst->unusable) continue;
        if (slot_of[r->instance] < 0) {
            if (!bake_reserve_modules(g_module_count + 1)) continue;
            slot_of[r->instance] = g_module_count;
            m = &g_modules[g_module_count++];
            memset(m, 0, sizeof *m);
            strncpy_s(m->module, sizeof m->module, inst->module, _TRUNCATE);
            m->instance = r->instance;
            m->ok = 1;
        }
        m = &g_modules[slot_of[r->instance]];
        m->regions++;
        if (!r->block_demons) m->not_floor++;
    }
    free(slot_of);
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
unsigned long sh_nav_bake_geometry_revision(void)
{
    unsigned long r;
    AcquireSRWLockShared(&g_bake_lock);
    r = g_geometry_revision;
    ReleaseSRWLockShared(&g_bake_lock);
    return r;
}

static const char *g_volumes_reason = "never tried";

const char *sh_nav_bake_volumes_reason(void) { return g_volumes_reason; }

/* A refused read keeps g_have_map but leaves g_loaded describing the read it threw away. */
static int bake_map_held_locked(void) { return g_have_map && !g_live_refused; }

int sh_nav_bake_volume_uids(int *out, int cap)
{
    int n;
    AcquireSRWLockShared(&g_bake_lock);
    n = bake_map_held_locked() ? sh_nav_regions_volume_uids(out, cap) : 0;
    ReleaseSRWLockShared(&g_bake_lock);
    return n;
}

int sh_nav_bake_knows_volume(int uid)
{
    int known;
    AcquireSRWLockShared(&g_bake_lock);
    known = bake_map_held_locked() && sh_nav_regions_knows_volume(uid);
    ReleaseSRWLockShared(&g_bake_lock);
    return known;
}

/* What went wrong with the last bake: `marked` is the volumes drawn red, and
 * `rooms` the rooms that produced no navigation. A room can fail with no volume
 * to blame, which is why both are counted. */
void sh_nav_bake_conflicts(int *marked, int *rooms)
{
    int i, m = 0, r = 0;
    AcquireSRWLockShared(&g_bake_lock);
    for (i = 0; i < g_module_count; i++) {
        m += g_preview[i].refused_count;
        /* Until the bake catches up, a room without bytes is still building. */
        if (g_preview_revision == g_geometry_revision &&
            g_modules[i].regions > 0 && !g_preview[i].bytes) r++;
    }
    ReleaseSRWLockShared(&g_bake_lock);
    if (marked) *marked = m;
    if (rooms)  *rooms  = r;
}

void sh_nav_bake_volume_counts(int *floors, int *walls)
{
    AcquireSRWLockShared(&g_bake_lock);
    if (floors) *floors = bake_map_held_locked() ? g_map.region_count : 0;
    if (walls)  *walls  = bake_map_held_locked() ? g_map.obstacle_count : 0;
    ReleaseSRWLockShared(&g_bake_lock);
}

int sh_nav_bake_has_map(void)
{
    int have;
    AcquireSRWLockShared(&g_bake_lock);
    have = bake_map_held_locked();
    ReleaseSRWLockShared(&g_bake_lock);
    return have;
}

int sh_nav_bake_instance_count(void)
{
    int n;
    AcquireSRWLockShared(&g_bake_lock);
    n = bake_map_held_locked() ? g_map.instance_count : 0;
    ReleaseSRWLockShared(&g_bake_lock);
    return n;
}

int sh_nav_bake_volume_flagged(int uid)
{
    sh_navr_entity_json get_json;
    void *ctx;
    AcquireSRWLockShared(&g_bake_lock);
    get_json = g_live_json;
    ctx = g_live_ctx;
    ReleaseSRWLockShared(&g_bake_lock);
    if (!get_json) return 0;
    return sh_nav_regions_volume_flagged(uid, get_json, ctx);
}

int sh_nav_bake_refresh_volumes(int *volumes)
{
    sh_nav_map before = {0};
    int count = 0, done = 0;
    const char *why = "read";
    if (volumes) *volumes = 0;
    if (InterlockedCompareExchange(&g_building, 0, 0)) {
        g_volumes_reason = "a bake is in progress";
        return 0;
    }
    AcquireSRWLockExclusive(&g_bake_lock);
    __try {
        if (!g_have_map || g_live_refused || !g_live_valid || !g_live_json) {
            why = "no map has been read yet"; __leave;
        }
        if (!sh_nav_map_copy(&before, &g_map)) { why = "allocation failed"; __leave; }
        if (!sh_nav_regions_refresh_known(&g_map, g_live_valid, g_live_json,
                                          g_live_transform, g_live_ctx,
                                          &count, &why)) __leave;
        g_live_marked = g_map.region_count;
        g_live_scanned = count;
        /* Order and entity indices aside, the same boxes bake the same. */
        if (!sh_nav_map_same_shapes(&before, &g_map)) {
            bake_log_change("the per-box refresh", &before, &g_map);
            g_geometry_revision++; bake_preview_clear(); bake_plan_locked();
        }
        if (volumes) *volumes = count;
        done = 1;
    } __finally { sh_nav_map_free(&before); ReleaseSRWLockExclusive(&g_bake_lock); }
    g_volumes_reason = done ? "read" : why;
    return done;
}

/* Say once per read what the map read left out, and why a read was refused. */
static void bake_note_read(const sh_nav_map *m)
{
    char line[256];
    if (m->invalid_geometry)
        backend_log("NAV: this map's room table (instanceEntities) is malformed, so no box "
                    "can be given a room; no custom navigation is available");
    if (m->truncated) {
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "NAV: the map holds more boxes or rooms than navigation reads "
                    "(%d boxes, %d rooms); %d box(es) past that were left out",
                    SH_NAVR_MAX_REGIONS, SH_NAVR_MAX_INSTANCES,
                    m->dropped_by[SH_NAVR_DROP_CAPACITY]);
        backend_log(line);
    }
    if (m->dropped_total) {
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "NAV: %d box(es) left out of the navigation, the rest of the map kept it",
                    m->dropped_total);
        backend_log(line);
    }
}

static void bake_refresh_live(int accept_unmeasured);
void sh_nav_bake_refresh_live(void) { bake_refresh_live(0); }
void sh_nav_bake_refresh_live_partial(void) { bake_refresh_live(1); }

static void bake_refresh_live(int accept_unmeasured)
{
    if (InterlockedCompareExchange(&g_building, 0, 0)) return;
    AcquireSRWLockExclusive(&g_bake_lock);
    __try {
    if (g_snapshot) {
        sh_nav_map *candidate = (sh_nav_map *)calloc(1, sizeof *candidate);
        int ok = candidate && g_snapshot(candidate, g_snapshot_ctx);
        if (!candidate) g_read_reason = "out of memory";
        else if (!ok) g_read_reason = "the map could not be serialized";
        if (ok) {
            if (candidate->ids_unusable != g_ids_unusable) {
                g_ids_unusable = candidate->ids_unusable;
                backend_log(g_ids_unusable
                    ? "NAV: two volumes share an id, which duplicating a box does "
                      "until the map is saved; reading the whole map each time"
                    : "NAV: every volume has its own id again");
            }
            /* A changed note is worth a line; the same one every read is not. */
            if (!g_have_map || g_live_refused ||
                candidate->dropped_total != g_map.dropped_total ||
                candidate->truncated != g_map.truncated || candidate->invalid_geometry)
                bake_note_read(candidate);
            /* An incomplete read is worth saying so once, and worth another
             * attempt rather than a refusal -- unless this read is the one a
             * build or Play waits for, which bakes what it could measure. */
            if (candidate->unmeasured && !accept_unmeasured)
                backend_log("NAV: the map had just opened and some boxes could not "
                            "be measured; reading it again");
            else if (candidate->unmeasured) {
                char line[160];
                _snprintf_s(line, sizeof line, _TRUNCATE,
                            "NAV: %d box(es) could not be measured and are left out of this "
                            "bake; the rest are built", candidate->unmeasured);
                backend_log(line);
            }
            if (candidate->invalid_geometry)
                g_read_reason = "its room table (instanceEntities) is malformed";
            else if (candidate->unmeasured)
                g_read_reason = "some boxes could not be measured yet";
            /* Boxes past a ceiling, or unreadable, are left out one at a time;
             * only a malformed room table refuses the read. */
            ok = !candidate->invalid_geometry && (!candidate->unmeasured || accept_unmeasured);
        }
        if (ok) {
            /* Two reads of one map list its boxes in different orders (the
             * editor's snapshot and the map Play converts): compare the boxes,
             * not the tables, so an unchanged map keeps its preview bakes. */
            int changed = !g_have_map || g_live_refused ||
                          !sh_nav_map_same_shapes(&g_map, candidate);
            if (changed && g_have_map && !g_live_refused)
                bake_log_change("the complete snapshot", &g_map, candidate);
            sh_nav_map_move(&g_map, candidate);
            /* The read attributed its volumes to `candidate`, which is about to be
             * freed. A later per-entity refresh reads g_map, so it inherits them. */
            sh_nav_regions_adopt(&g_map);
            g_have_map = 1;
            g_live_marked = g_map.region_count;
            g_live_scanned = g_map.region_count;
            g_live_refused = 0;
            g_read_reason = "";
            if (changed) { g_geometry_revision++; bake_preview_clear(); bake_plan_locked(); }
        } else {
            /* An unreadable current snapshot must never reuse moved or deleted
             * geometry from an earlier edit generation. */
            if (!g_live_refused) g_geometry_revision++;
            g_live_refused = 1;
            g_module_count = 0;
            bake_preview_clear();
        }
        if (candidate) sh_nav_map_free(candidate);
        free(candidate);
    } else if (g_have_map) bake_refresh_live_locked();
    } __finally { ReleaseSRWLockExclusive(&g_bake_lock); }
}

/* Is this class of this copy already known to refuse at the current geometry?
 * The monster48 preview counts: the worker baked exactly what Play would. */
static int bake_class_refused_locked(const bake_module *m, int ci)
{
    int slot = (int)(m - g_modules);
    if (ci < 0 || ci >= NAV_CLASS_COUNT) return 0;
    if (m->cls[ci].state == SH_NAV_CLASS_REFUSED && m->cls[ci].revision == g_geometry_revision)
        return 1;
    return ci == 0 && g_preview_revision == g_geometry_revision &&
           g_preview[slot].done && !g_preview[slot].bytes;
}

/* Private resource names are scoped by both edit revision and exact instance.
 * The engine uses them only for a temporary load, then releases the resource.
 * A class already known to refuse gets no private name, so the engine loads
 * the module's shipped navigation for it without a second attempt. */
int sh_nav_bake_instance_name(int instance,const char *name,char *out,size_t capacity)
{
    char module[SH_NAVR_MODULE_CAP],cls[32];int i,ok=0,ci;
    if(!g_instance_serving||!name||!out||!capacity||!bake_enabled()||
       InterlockedCompareExchange(&g_faulted,0,0))return 0;
    if(!bake_parse_name(name,module,sizeof module,cls,sizeof cls))return 0;
    ci=bake_class_index(cls);
    AcquireSRWLockShared(&g_bake_lock);
    if(g_have_map&&!g_live_refused&&instance>=0&&instance<g_map.instance_count&&
       !strcmp(module,g_map.instances[instance].module)) {
        for(i=0;i<g_module_count;i++)if(g_modules[i].instance==instance&&g_modules[i].ok) {
            if(bake_class_refused_locked(&g_modules[i],ci))break;
            {
                int n=_snprintf_s(out,capacity,_TRUNCATE,"maps/smpnav/%lu/%d/%s",
                    g_geometry_revision,instance,name+5);
                ok=n>0;
            }
            break;
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
    sh_nav_map_free(&g_map);
    bake_preview_clear();g_geometry_revision++;
    if (g_modules) memset(g_modules, 0, (size_t)g_module_cap * sizeof *g_modules);
    g_module_count = 0;
    g_have_map = 0;
    g_census_count = 0;
    g_live_scanned = -1;
    g_live_marked = 0;
    g_live_refused = 0;
    g_read_reason = "";

    if (bake_enabled() && json && len) {
        __try {
            /* Map text omits sizes equal to the entityDef's; read those from
             * the entityDef so a box the author never resized keeps its shape
             * after a load or a Save. */
            ok = sh_nav_regions_read_defaults(json, len, &g_map,
                                              sh_nav_entitydef_default_size, NULL);
            if (ok) bake_note_read(&g_map);
            if (ok && !g_map.invalid_geometry && !g_map.unmeasured) {
                bake_plan_locked();
                g_have_map = 1;
            } else {
                g_live_refused = 1;
                g_read_reason = !ok ? "the map text could not be parsed"
                                    : "its room table (instanceEntities) is malformed";
            }
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

/* The boxes one copy offers the augmenter: its marked boxes that block demons
 * (floors), then its Block Demons boxes (walls that only block). A heap array
 * the caller frees; at most SH_AUG_MAX_PLATFORMS, with *left_out counting the
 * boxes past that ceiling and *not_floor the marked boxes demons fall through. */
static sh_aug_platform *bake_collect_platforms(const bake_module *m, int *count,
                                               int *left_out, int *not_floor)
{
    sh_aug_platform *out;
    int i, n = 0, want = 0;
    *count = 0; *left_out = 0; *not_floor = 0;
    for (i = 0; i < g_map.region_count; i++) {
        const sh_nav_region *r = &g_map.regions[i];
        if (r->instance != m->instance) continue;
        if (r->block_demons) want++; else (*not_floor)++;
    }
    for (i = 0; i < g_map.obstacle_count; i++)
        if (g_map.obstacles[i].instance == m->instance && g_map.obstacles[i].block_demons) want++;
    if (want > SH_AUG_MAX_PLATFORMS) { *left_out = want - SH_AUG_MAX_PLATFORMS; want = SH_AUG_MAX_PLATFORMS; }
    if (!want) return NULL;
    out = (sh_aug_platform *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                       (size_t)want * sizeof *out);
    if (!out) return NULL;
    for (i = 0; i < g_map.region_count && n < want; i++) {
        const sh_nav_region *r = &g_map.regions[i];
        if (r->instance != m->instance) continue;
        /* Only geometry that blocks demons can support a walkable surface. */
        if (!r->block_demons) continue;
        memcpy(out[n].c, r->c, sizeof r->c);
        memcpy(out[n].n, r->n, sizeof out[n].n);
        out[n].face = r->face;
        out[n].depth = r->depth;
        _snprintf_s(out[n].name, sizeof out[n].name, _TRUNCATE, "volume %u", r->entity);
        n++;
    }
    for(i=0;i<g_map.obstacle_count&&n<want;i++) {
        const sh_nav_region *r=&g_map.obstacles[i];
        if(r->instance!=m->instance||!r->block_demons)continue;
        memcpy(out[n].c,r->c,sizeof r->c);memcpy(out[n].n,r->n,sizeof r->n);
        out[n].face=r->face;out[n].depth=r->depth;out[n].obstacle_only=1;
        _snprintf_s(out[n].name,sizeof out[n].name,_TRUNCATE,"obstacle %u",r->entity);n++;
    }
    *count = n;
    return out;
}

/* Everything one class bake produced, refused or not. */
typedef struct bake_outcome {
    unsigned char *bytes;               /* process heap; NULL when refused */
    size_t         length;
    unsigned       first_area;
    int            state;               /* SH_NAV_CLASS_* */
    char           reason[BAKE_REASON_CAP];
    char           brief[BAKE_BRIEF_CAP];
    char           faces[BAKE_FACES_CAP];
    int            faces_climbed, faces_total;
    bake_refused   refused[BAKE_MAX_REFUSED];   /* red: what to move to fit */
    int            refused_count;
    bake_refused   partial[BAKE_MAX_REFUSED];   /* amber: what lost something */
    int            partial_count;
} bake_outcome;

static void bake_append(char *text, size_t cap, const char *fmt, ...)
{
    size_t at = strlen(text);
    va_list ap;
    if (at + 1 >= cap) return;
    va_start(ap, fmt);
    _vsnprintf_s(text + at, cap - at, _TRUNCATE, fmt, ap);
    va_end(ap);
}

static void bake_mark(bake_refused *list, int *count, const sh_aug_platform *plats,
                      int plat_count, int src)
{
    int i;
    if (src < 0 || src >= plat_count || *count >= BAKE_MAX_REFUSED) return;
    for (i = 0; i < *count; i++)
        if (!memcmp(list[i].c, plats[src].c, sizeof list[i].c)) return;
    memcpy(list[*count].c, plats[src].c, sizeof list[0].c);
    (*count)++;
}

/* The whole log story of a refusal: what was offered, what had been placed,
 * which volumes made no floor and, for a route refusal, which volumes route
 * into the full area. */
static void bake_log_refusal(const char *name, const bake_outcome *o,
                             const sh_aug_platform *plats, int plat_count,
                             const sh_aug_report *rep)
{
    char line[512];
    int i, emitted = 0, obstacles = 0, shown = 0;

    for (i = 0; i < rep->platform_count; i++)
        if (rep->platforms[i].emitted) emitted++;
    for (i = 0; i < plat_count; i++)
        if (plats[i].obstacle_only) obstacles++;

    _snprintf_s(line, sizeof line, _TRUNCATE,
                "NAV: refused '%s' -- %s", name, o->reason);
    backend_log(line);
    _snprintf_s(line, sizeof line, _TRUNCATE,
                "NAV:   %d volume(s) offered (%d of them walls that only block), "
                "%d walkable piece(s) had been placed, areas %u->%u, links %u->%u, "
                "tree depth %u->%u",
                plat_count, obstacles, emitted,
                rep->areas_before, rep->areas_after,
                rep->reach_before, rep->reach_after,
                rep->depth_before, rep->depth_after);
    backend_log(line);

    /* Volumes that produced no floor. A successful bake has these too, so
     * they are context, not the cause. Eight is enough to see a pattern. */
    for (i = 0; i < rep->platform_count && shown < 8; i++) {
        if (rep->platforms[i].emitted) continue;
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "NAV:   %s made no walkable floor -- %s",
                    rep->platforms[i].name,
                    rep->platforms[i].reason[0] ? rep->platforms[i].reason : "no reason recorded");
        backend_log(line);
        shown++;
    }

    if (!rep->reach_limit_exceeded) return;
    _snprintf_s(line, sizeof line, _TRUNCATE,
                "NAV:   area %d already carries %u routes out of it in the module's own data",
                rep->reach_limit_area, SH_AAS_MAX_AREA_REACHABILITIES);
    backend_log(line);
    for (i = 0; i < rep->blamed_count; i++) {
        int src = rep->blamed[i];
        if (src < 0 || src >= plat_count) continue;
        _snprintf_s(line, sizeof line, _TRUNCATE,
                    "NAV:   %s contributes routes from area %d",
                    plats[src].name, rep->reach_limit_area);
        backend_log(line);
    }
}

/* "volume 2: N 9/9, E 9/9, S none (blocked), W 4/9; ..." -- for each box
 * that offers a top, each top edge by the compass of its outward normal in
 * module space, with the demons that have a floor climb onto it out of every
 * demon with animations, or the commonest reason none was placed. */
static void bake_faces_text(const sh_aug_report *rep, const sh_aug_platform *plats, int count,
                            char *out, size_t cap, int *climbed, int *total)
{
    static const char *const why[SH_AUG_FACE_REASONS] = {
        "blocked", "no floor", "edge", "too high", "inside", "no floor edge" };
    static const char *const compass[8] = { "E", "NE", "N", "NW", "W", "SW", "S", "SE" };
    int s, e, demons = sh_trav_monster_count();
    out[0] = 0; *climbed = 0; *total = 0;
    if (!rep->faces || demons <= 0) return;
    for (s = 0; s < count && s < rep->source_count; s++) {
        if (plats[s].obstacle_only) continue;
        bake_append(out, cap, "%s%s:", out[0] ? "; " : "", plats[s].name);
        for (e = 0; e < 4; e++) {
            const sh_aug_face *f = &rep->faces[s * 4 + e];
            const float *a = plats[s].c[e], *b = plats[s].c[(e + 1) % 4];
            double ang = atan2(b[0] - a[0], -(b[1] - a[1])) * 180.0 / 3.14159265358979323846;
            int dir = (int)floor(fmod(ang + 360.0 + 22.5, 360.0) / 45.0) & 7, m, bits = 0, best = -1;
            for (m = 0; m < 32; m++) if (f->demons & (1u << m)) bits++;
            (*total)++;
            if (f->climbs) {
                (*climbed)++;
                bake_append(out, cap, "%s %s %d/%d", e ? "," : "", compass[dir], bits, demons);
                continue;
            }
            if (f->walks) { (*climbed)++; bake_append(out, cap, "%s %s walk", e ? "," : "", compass[dir]); continue; }
            for (m = 0; m < SH_AUG_FACE_REASONS; m++)
                if (f->reasons[m] && (best < 0 || f->reasons[m] > f->reasons[best])) best = m;
            bake_append(out, cap, "%s %s none (%s)", e ? "," : "", compass[dir],
                        why[best >= 0 ? best : SH_AUG_FACE_NO_EDGE]);
        }
    }
}

/* Bake one class of one copy from its gathered inputs. Touches nothing but
 * its arguments, so the worker runs it as well as BuildAAS. */
static void bake_run(const char *name, const sh_aug_platform *in_plats, int in_count,
                     const unsigned char *shipped, size_t shipped_len,
                     int left_out, int not_floor, bake_outcome *o)
{
    sh_aas *model;
    sh_aug_opts opts;
    sh_aug_report rep;
    char err[192];
    int i, floors = 0, keep, tries = 0, depth_cut = 0, depth_first = 0;
    /* The payload phase: lower the route budget, then shed floors, until the
     * file fits sh_aas_payload_cap(). size_first is the size the full set
     * would have had; routes_engine what the 256 limit alone sheds. */
    int budget = 0, size_tries = 0, size_floor_tries = 0, size_keep = -1;
    int routes_engine = -1, size_cut = 0;
    size_t size_first = 0, size_last = 0, cap = sh_aas_payload_cap();
    const sh_aug_platform *plats = in_plats;
    int plat_count = in_count;
    sh_aug_platform *subset = NULL;

    memset(o, 0, sizeof *o);
    memset(&rep, 0, sizeof rep);
    o->state = SH_NAV_CLASS_REFUSED;
    if (plat_count == 0) {
        if (not_floor)
            _snprintf_s(o->reason, sizeof o->reason, _TRUNCATE,
                        "%d marked box(es) do not block demons, so none is a floor; "
                        "turn on Block Demons for them", not_floor);
        else
            _snprintf_s(o->reason, sizeof o->reason, _TRUNCATE,
                        "no marked volume also blocks demons, so none is a floor");
        strcpy_s(o->brief, sizeof o->brief, "no marked box blocks demons");
        return;
    }
    if (!shipped) {
        _snprintf_s(o->reason, sizeof o->reason, _TRUNCATE,
                    "the module's own navigation could not be read back");
        strcpy_s(o->brief, sizeof o->brief, "its own navigation could not be read");
        return;
    }
    /* Floors come first in the offered boxes, walls that only block after. */
    while (floors < in_count && !in_plats[floors].obstacle_only) floors++;
    keep = floors;
    memset(&opts, 0, sizeof opts);
    opts.fall = SH_AUG_FALL_AUTO;
    opts.inset = 1;
    opts.traversal = 0;
    for (;;) {
        opts.route_budget = budget;
        int augmented;
        err[0] = 0;
        model = sh_aas_parse(shipped, shipped_len, err, sizeof err);
        if (!model) {
            _snprintf_s(o->reason, sizeof o->reason, _TRUNCATE,
                        "the module's navigation did not parse: %s", err);
            strcpy_s(o->brief, sizeof o->brief, "its own navigation did not parse");
            if (subset) HeapFree(GetProcessHeap(), 0, subset);
            return;
        }
        o->first_area = sh_aas_count(model, SH_AAS_L_AREAS);
        SH_PERF_BEGIN(ta);
        augmented = sh_aas_augment(model, plats, plat_count, &opts, &rep);
        SH_PERF_END(SH_PERF_BAKE_AUGMENT, ta);
        /* Pieces of the cut module floor come first among the added areas;
         * the preview draws only what stands on the boxes. */
        o->first_area += (unsigned)rep.floor_pieces_added;
        /* The loader refuses a tree past 128 deep. Rather than lose the copy,
         * keep three quarters of the floors (every wall stays) and try again,
         * a few times at most; what was left out is reported. */
        if (augmented && rep.depth_exceeded && keep > 1 && tries < 6) {
            sh_aas_free(model);
            if (!tries) depth_first = (int)rep.depth_after;
            sh_aug_report_free(&rep);
            if (!subset) subset = (sh_aug_platform *)HeapAlloc(GetProcessHeap(), 0,
                                      (size_t)in_count * sizeof *subset);
            if (!subset) {
                strcpy_s(o->reason, sizeof o->reason, "out of memory while retrying with fewer boxes");
                strcpy_s(o->brief, sizeof o->brief, "out of memory");
                break;
            }
            keep = keep * 3 / 4;
            if (keep < 1) keep = 1;
            memcpy(subset, in_plats, (size_t)keep * sizeof *subset);
            memcpy(subset + keep, in_plats + floors, (size_t)(in_count - floors) * sizeof *subset);
            plats = subset;
            plat_count = keep + (in_count - floors);
            tries++;
            continue;
        }
        if (augmented && rep.depth_exceeded) {
            _snprintf_s(o->reason, sizeof o->reason, _TRUNCATE,
                        "the navigation tree would be %u deep and the engine loads at most "
                        "%u; this copy has %d box(es), spread or merge some of them",
                        tries ? (unsigned)depth_first : rep.depth_after,
                        (unsigned)SH_AAS_MAX_DEPTH, in_count);
            strcpy_s(o->brief, sizeof o->brief, "navigation tree deeper than 128");
        } else if (augmented) {
            size_t size = sh_aas_serialized_size(model);
            if (routes_engine < 0) routes_engine = rep.routes_dropped;
            if (size > cap) {
                /* A dense copy outgrows the file before the tree or the route
                 * limit: thin its longest routes first, then its floors. */
                int next = size_tries < 4
                         ? sh_aug_route_budget_for(model, &rep, size, cap, budget) : 0;
                if (!size_first) { size_first = size; size_keep = keep; }
                size_last = size;
                if (next > 0 || (keep > 1 && size_floor_tries < 6)) {
                    sh_aas_free(model);
                    sh_aug_report_free(&rep);
                    if (next > 0) { budget = next; size_tries++; continue; }
                    if (!subset) subset = (sh_aug_platform *)HeapAlloc(GetProcessHeap(), 0,
                                              (size_t)in_count * sizeof *subset);
                    if (!subset) {
                        strcpy_s(o->reason, sizeof o->reason,
                                 "out of memory while retrying with fewer boxes");
                        strcpy_s(o->brief, sizeof o->brief, "out of memory");
                        break;
                    }
                    keep = keep * 3 / 4;
                    if (keep < 1) keep = 1;
                    memcpy(subset, in_plats, (size_t)keep * sizeof *subset);
                    memcpy(subset + keep, in_plats + floors,
                           (size_t)(in_count - floors) * sizeof *subset);
                    plats = subset;
                    plat_count = keep + (in_count - floors);
                    size_floor_tries++;
                    continue;
                }
                _snprintf_s(o->reason, sizeof o->reason, _TRUNCATE,
                            "the navigation file would be %.1f MiB and at most %.1f MiB is "
                            "served per room; with every long route and all but one floor "
                            "left out it was still %.1f MiB",
                            size_first / 1048576.0, cap / 1048576.0, size_last / 1048576.0);
                _snprintf_s(o->brief, sizeof o->brief, _TRUNCATE,
                            "navigation file over %.0f MiB", cap / 1048576.0);
            } else {
                o->bytes = sh_aas_write(model, &o->length);
                if (size_keep >= 0) {
                    depth_cut = floors - size_keep;
                    size_cut = size_keep - keep;
                } else depth_cut = floors - keep;
            }
        }
        sh_aas_free(model);
        break;
    }

    if (!o->bytes) {
        if (!o->reason[0]) {
            if (rep.reach_limit_exceeded) {
                _snprintf_s(o->reason, sizeof o->reason, _TRUNCATE,
                            "the module's own navigation already has %u routes out of area "
                            "%d, the engine's limit, so nothing generated can join it there",
                            SH_AAS_MAX_AREA_REACHABILITIES, rep.reach_limit_area);
                strcpy_s(o->brief, sizeof o->brief, "its own routes already fill one area");
                for (i = 0; i < rep.blamed_count; i++)
                    bake_mark(o->refused, &o->refused_count, plats, plat_count, rep.blamed[i]);
            } else if (rep.links_truncated) {
                strcpy_s(o->reason, sizeof o->reason, "route storage could not be allocated");
                strcpy_s(o->brief, sizeof o->brief, "out of memory");
            } else if (rep.pieces_truncated) {
                strcpy_s(o->reason, sizeof o->reason,
                         "a marked box's geometry could not be cut into walkable pieces");
                strcpy_s(o->brief, sizeof o->brief, "a box's geometry could not be cut");
            } else if (rep.segments_failed) {
                strcpy_s(o->reason, sizeof o->reason,
                         "an edge could not be measured against the module's navigation");
                strcpy_s(o->brief, sizeof o->brief, "an edge could not be measured");
            } else {
                strcpy_s(o->reason, sizeof o->reason, "the bake did not produce a payload");
                strcpy_s(o->brief, sizeof o->brief, "the bake produced nothing");
            }
        }
        bake_log_refusal(name, o, plats, plat_count, &rep);
        sh_aug_report_free(&rep);
        if (subset) HeapFree(GetProcessHeap(), 0, subset);
        return;
    }

    /* Generated output must pass the same structural gate as embedded payloads. */
    err[0] = 0;
    if (!sh_navmesh_validate_aas(o->bytes, o->length, err, sizeof err)) {
        HeapFree(GetProcessHeap(), 0, o->bytes);
        o->bytes = NULL; o->length = 0;
        _snprintf_s(o->reason, sizeof o->reason, _TRUNCATE,
                    "the bake did not pass validation: %s", err);
        strcpy_s(o->brief, sizeof o->brief, "the result failed validation");
        bake_log_refusal(name, o, plats, plat_count, &rep);
        sh_aug_report_free(&rep);
        if (subset) HeapFree(GetProcessHeap(), 0, subset);
        return;
    }

    o->state = SH_NAV_CLASS_BUILT;
    {
        /* Report islands separately from surfaces with incoming routes. */
        int islands = 0, climbs = 0, leaps = 0, chained = 0, tipped = 0;
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
        _snprintf_s(o->reason, sizeof o->reason, _TRUNCATE,
                    "%d platform(s), areas %u->%u, links %u->%u "
                    "(%d climb%s, %d leap%s, %d chained), tree depth %u->%u%s%s%s",
                    plat_count, rep.areas_before, rep.areas_after,
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
        /* What was left out so the rest fits comes first among the notes:
         * it is the part of the result the author asked for and did not get. */
        if (depth_cut) {
            bake_append(o->reason, sizeof o->reason,
                        "; %d floor box(es) left out to keep the navigation tree within the "
                        "engine's %u (it would have been %d deep)", depth_cut,
                        (unsigned)SH_AAS_MAX_DEPTH, depth_first);
            _snprintf_s(o->brief, sizeof o->brief, _TRUNCATE,
                        "%d box(es) left out at the 128 tree-depth limit", depth_cut);
            for (i = size_keep >= 0 ? size_keep : keep;
                 i < floors && o->partial_count < BAKE_MAX_REFUSED; i++)
                bake_mark(o->partial, &o->partial_count, in_plats, in_count, i);
            o->state = SH_NAV_CLASS_PARTIAL;
            {
                char line[SH_SMNAV_RESNAME_CAP + 160];
                _snprintf_s(line, sizeof line, _TRUNCATE,
                            "NAV:   '%s': the last %d of %d floor box(es) left out to keep the "
                            "tree within %u", name, depth_cut, floors, (unsigned)SH_AAS_MAX_DEPTH);
                backend_log(line);
            }
        }
        if (size_first) {
            /* The routes the 256 limit alone would have shed are reported
             * as such; the rest were thinned for the file size. */
            int thinned = rep.routes_dropped - (routes_engine > 0 ? routes_engine : 0);
            if (thinned < 0) thinned = 0;
            bake_append(o->reason, sizeof o->reason,
                        "; %d long route(s) left out (each area keeps at most %d)",
                        thinned, budget ? budget : (int)SH_AAS_MAX_AREA_REACHABILITIES);
            if (size_cut)
                bake_append(o->reason, sizeof o->reason, " and the last %d floor box(es)",
                            size_cut);
            bake_append(o->reason, sizeof o->reason,
                        " to keep the navigation file within %.0f MiB (it would have been "
                        "%.1f MiB)", cap / 1048576.0, size_first / 1048576.0);
            if (!o->brief[0] && size_cut)
                _snprintf_s(o->brief, sizeof o->brief, _TRUNCATE,
                            "%d route(s), %d box(es) left out at the %.0f MiB file limit",
                            thinned, size_cut, cap / 1048576.0);
            else if (!o->brief[0])
                _snprintf_s(o->brief, sizeof o->brief, _TRUNCATE,
                            "%d route(s) left out at the %.0f MiB file limit",
                            thinned, cap / 1048576.0);
            for (i = keep; i < (size_keep >= 0 ? size_keep : keep) &&
                           o->partial_count < BAKE_MAX_REFUSED; i++)
                bake_mark(o->partial, &o->partial_count, in_plats, in_count, i);
            o->state = SH_NAV_CLASS_PARTIAL;
            {
                char line[SH_SMNAV_RESNAME_CAP + 200];
                _snprintf_s(line, sizeof line, _TRUNCATE,
                            "NAV:   '%s': the navigation file would have been %.1f MiB, over the "
                            "%.0f MiB served per room; %d long route(s) and %d floor box(es) "
                            "left out, %.1f MiB built", name, size_first / 1048576.0,
                            cap / 1048576.0, thinned, size_cut, o->length / 1048576.0);
                backend_log(line);
            }
        }
        if (rep.routes_dropped) {
            if (!size_first)
                bake_append(o->reason, sizeof o->reason,
                            "; %d route(s) left out so %d area(s) stay within the engine's %u",
                            rep.routes_dropped, rep.route_areas, SH_AAS_MAX_AREA_REACHABILITIES);
            if (!o->brief[0])
                _snprintf_s(o->brief, sizeof o->brief, _TRUNCATE,
                            "%d route(s) left out at the 256-per-area limit", rep.routes_dropped);
            for (i = 0; i < rep.dropped_count; i++)
                bake_mark(o->partial, &o->partial_count, plats, plat_count, rep.dropped[i].source);
            o->state = SH_NAV_CLASS_PARTIAL;
        }
        if (rep.sources_dropped) {
            bake_append(o->reason, sizeof o->reason,
                        "; %d box(es) lost a face at the %d-piece limit",
                        rep.sources_dropped, SH_AUG_MAX_PLATFORMS);
            if (!o->brief[0])
                _snprintf_s(o->brief, sizeof o->brief, _TRUNCATE,
                            "%d box(es) left out at the %d-piece limit",
                            rep.sources_dropped, SH_AUG_MAX_PLATFORMS);
            for (i = 0; i < rep.dropped_source_count; i++)
                bake_mark(o->partial, &o->partial_count, plats, plat_count, rep.dropped_sources[i]);
            o->state = SH_NAV_CLASS_PARTIAL;
        }
        if (left_out) {
            bake_append(o->reason, sizeof o->reason,
                        "; %d box(es) past %d in this room left out", left_out,
                        SH_AUG_MAX_PLATFORMS);
            if (!o->brief[0])
                _snprintf_s(o->brief, sizeof o->brief, _TRUNCATE,
                            "%d box(es) past %d per room left out", left_out,
                            SH_AUG_MAX_PLATFORMS);
            o->state = SH_NAV_CLASS_PARTIAL;
        }
        if (rep.file_full) {
            bake_append(o->reason, sizeof o->reason,
                        "; the module's navigation file filled, later pieces left out");
            if (!o->brief[0]) strcpy_s(o->brief, sizeof o->brief, "navigation file full");
            o->state = SH_NAV_CLASS_PARTIAL;
        }
        if (rep.edges_skipped)
            bake_append(o->reason, sizeof o->reason,
                        "; %d edge(s) too intricate to link", rep.edges_skipped);
        if (not_floor)
            bake_append(o->reason, sizeof o->reason,
                        "; %d marked box(es) without Block Demons are not floors", not_floor);
        if (rep.anchors_reduced)
            bake_append(o->reason, sizeof o->reason,
                        "; %d alternative link samples removed to fit native routing",
                        rep.anchors_reduced);
        if (cut)
            bake_append(o->reason, sizeof o->reason,
                        "; %d volume(s) cut around the solids standing in them", cut);
        if (rep.floors_cut)
            bake_append(o->reason, sizeof o->reason,
                        "; floor cut around the boxes on it (%d area(s), %d piece(s) added)",
                        rep.floors_cut, rep.floor_pieces_added);
        if (rep.floors_left_whole)
            bake_append(o->reason, sizeof o->reason,
                        "; %d floor area(s) under too many boxes left whole", rep.floors_left_whole);
        if (rep.floors_covered)
            bake_append(o->reason, sizeof o->reason,
                        "; %d floor area(s) wholly under boxes closed", rep.floors_covered);
        /* Keep optional notes last so truncation preserves the cut-volume count. */
        if (rep.dead_gaps)
            bake_append(o->reason, sizeof o->reason,
                        "; %d pair(s) too far to step and too close to jump", rep.dead_gaps);
        bake_faces_text(&rep, plats, plat_count, o->faces, sizeof o->faces,
                        &o->faces_climbed, &o->faces_total);
        if (o->state == SH_NAV_CLASS_PARTIAL) {
            char line[SH_SMNAV_RESNAME_CAP + 160];
            for (i = 0; i < rep.dropped_count && i < 8; i++) {
                int src = rep.dropped[i].source;
                _snprintf_s(line, sizeof line, _TRUNCATE,
                            "NAV:   '%s': %s lost %d route(s) out of area %d (%s)", name,
                            src >= 0 && src < plat_count ? plats[src].name : "the module floor",
                            rep.dropped[i].routes, rep.dropped[i].area,
                            size_first ? "file size" : "256 limit");
                backend_log(line);
            }
            for (i = 0; i < rep.dropped_source_count; i++) {
                int src = rep.dropped_sources[i];
                if (src < 0 || src >= plat_count) continue;
                _snprintf_s(line, sizeof line, _TRUNCATE,
                            "NAV:   '%s': %s lost a walkable face at the piece limit",
                            name, plats[src].name);
                backend_log(line);
            }
        }
    }
    sh_aug_report_free(&rep);
    if (subset) HeapFree(GetProcessHeap(), 0, subset);
}

/* ==================================================================== */
/* the bake cache                                                        */
/* ==================================================================== */

/* Successful class bakes, kept across Plays, map reloads and launches so a copy
 * whose boxes have not changed is served instead of baked again. An entry is
 * keyed by what bake_run reads: the module and class, the shipped file (its
 * length and a 64-bit hash), whether a traversal table was loaded, and the
 * copy's room and boxes. Boxes are compared with sh_nav_map_same_shapes, the
 * comparison the preview uses, on a one-room map of that copy alone, so an edit
 * to one copy leaves every other copy's entries matching.
 *
 * Memory: at most BAKE_CACHE_BUDGET bytes of payloads, box tables and marks,
 * and BAKE_CACHE_MAX_ENTRIES entries. A new entry evicts the least recently
 * used ones, but never one used since the current pass began (a Play or a
 * preview gather): when everything the current pass needs does not fit, the
 * newest bake is not kept rather than evicting one this same pass serves, so
 * a map larger than the budget keeps a stable part of itself cached instead of
 * every entry pushing out the next. Refusals are not kept here; the
 * per-revision refusal memory in bake_class handles them. */
#define BAKE_CACHE_BUDGET      ((size_t)128 * 1024 * 1024)
#define BAKE_CACHE_MAX_ENTRIES 1024

typedef struct bake_cache_key {
    char               module[SH_NAVR_MODULE_CAP];
    int                cls;
    unsigned long long shipped_hash;
    size_t             shipped_len;
    int                traversal;
    sh_nav_map         shapes;     /* one room, index 0, and its boxes */
} bake_cache_key;

typedef struct bake_cache_entry {
    bake_cache_key     key;
    unsigned char     *bytes;      /* process heap */
    size_t             length;
    unsigned           first_area;
    int                state;
    char               reason[BAKE_REASON_CAP];
    char               brief[BAKE_BRIEF_CAP];
    char               faces[BAKE_FACES_CAP];
    int                faces_climbed, faces_total;
    bake_refused       partial[BAKE_MAX_REFUSED];
    int                partial_count;
    size_t             cost;
    unsigned long long used;       /* g_cache_clock at the last store or hit */
} bake_cache_entry;

static bake_cache_entry  **g_cache;
static int                 g_cache_count;
static size_t              g_cache_bytes;
static size_t              g_cache_budget = BAKE_CACHE_BUDGET;
static unsigned long long  g_cache_clock, g_cache_pass;
static unsigned long       g_cache_hits, g_cache_stores, g_cache_evicted, g_cache_declined;
static int                 g_cache_full_logged;

static unsigned long long bake_hash(const unsigned char *p, size_t n)
{
    unsigned long long h = 1469598103934665603ULL;
    size_t i;
    for (i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

static void bake_cache_key_free(bake_cache_key *k) { sh_nav_map_free(&k->shapes); }

/* The copy's room and every box it owns, renumbered as room 0. */
static int bake_copy_shapes(const bake_module *m, sh_nav_map *out)
{
    int i, nr = 0, no = 0;
    memset(out, 0, sizeof *out);
    if (m->instance < 0 || m->instance >= g_map.instance_count) return 0;
    for (i = 0; i < g_map.region_count; i++) if (g_map.regions[i].instance == m->instance) nr++;
    for (i = 0; i < g_map.obstacle_count; i++) if (g_map.obstacles[i].instance == m->instance) no++;
    out->instances = (sh_nav_instance *)malloc(sizeof *out->instances);
    out->regions = (sh_nav_region *)malloc((size_t)(nr ? nr : 1) * sizeof *out->regions);
    out->obstacles = (sh_nav_region *)malloc((size_t)(no ? no : 1) * sizeof *out->obstacles);
    if (!out->instances || !out->regions || !out->obstacles) { sh_nav_map_free(out); return 0; }
    out->instances[0] = g_map.instances[m->instance];
    out->instance_count = out->instance_cap = 1;
    out->region_cap = nr ? nr : 1; out->obstacle_cap = no ? no : 1;
    for (i = 0; i < g_map.region_count; i++)
        if (g_map.regions[i].instance == m->instance) {
            out->regions[out->region_count] = g_map.regions[i];
            out->regions[out->region_count++].instance = 0;
        }
    for (i = 0; i < g_map.obstacle_count; i++)
        if (g_map.obstacles[i].instance == m->instance) {
            out->obstacles[out->obstacle_count] = g_map.obstacles[i];
            out->obstacles[out->obstacle_count++].instance = 0;
        }
    return 1;
}

static int bake_cache_key_make(bake_cache_key *k, const bake_module *m, int ci,
                               const unsigned char *shipped, size_t shipped_len,
                               int traversal)
{
    memset(k, 0, sizeof *k);
    strncpy_s(k->module, sizeof k->module, m->module, _TRUNCATE);
    k->cls = ci;
    k->shipped_len = shipped ? shipped_len : 0;
    k->shipped_hash = shipped ? bake_hash(shipped, shipped_len) : 0;
    k->traversal = traversal;
    return bake_copy_shapes(m, &k->shapes);
}

/* Called with the bake lock held. `k->shapes` may be reordered by a match. */
static bake_cache_entry *bake_cache_find(bake_cache_key *k)
{
    int i;
    for (i = 0; i < g_cache_count; i++) {
        bake_cache_entry *e = g_cache[i];
        if (e->key.cls != k->cls || e->key.shipped_len != k->shipped_len ||
            e->key.shipped_hash != k->shipped_hash || e->key.traversal != k->traversal ||
            e->key.shapes.region_count != k->shapes.region_count ||
            e->key.shapes.obstacle_count != k->shapes.obstacle_count ||
            strcmp(e->key.module, k->module))
            continue;
        if (sh_nav_map_same_shapes(&e->key.shapes, &k->shapes)) {
            e->used = ++g_cache_clock;
            return e;
        }
    }
    return NULL;
}

static void bake_cache_drop(int i)
{
    bake_cache_entry *e = g_cache[i];
    g_cache_bytes -= e->cost;
    if (e->bytes) HeapFree(GetProcessHeap(), 0, e->bytes);
    bake_cache_key_free(&e->key);
    free(e);
    g_cache[i] = g_cache[--g_cache_count];
}

static void bake_cache_clear(void)
{
    while (g_cache_count) bake_cache_drop(g_cache_count - 1);
    free(g_cache); g_cache = NULL;
    g_cache_bytes = 0; g_cache_full_logged = 0;
}

/* A new pass (a Play, or a preview gather) starts: what it uses is protected. */
static void bake_cache_begin_pass(void)
{
    g_cache_pass = ++g_cache_clock;
    g_cache_full_logged = 0;
}

/* Fill an outcome from an entry, as bake_run would have. 0 on allocation failure. */
static int bake_cache_serve(const bake_cache_entry *e, bake_outcome *o)
{
    memset(o, 0, sizeof *o);
    o->bytes = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, e->length ? e->length : 1);
    if (!o->bytes) return 0;
    memcpy(o->bytes, e->bytes, e->length);
    o->length = e->length;
    o->first_area = e->first_area;
    o->state = e->state;
    strcpy_s(o->reason, sizeof o->reason, e->reason);
    strcpy_s(o->brief, sizeof o->brief, e->brief);
    strcpy_s(o->faces, sizeof o->faces, e->faces);
    o->faces_climbed = e->faces_climbed; o->faces_total = e->faces_total;
    memcpy(o->partial, e->partial, sizeof o->partial);
    o->partial_count = e->partial_count;
    g_cache_hits++;
    return 1;
}

/* Keep a successful outcome under `k`, whose shapes move into the entry. */
static void bake_cache_store(bake_cache_key *k, const bake_outcome *o)
{
    bake_cache_entry *e;
    size_t cost;
    if (!o->bytes || o->state == SH_NAV_CLASS_REFUSED) return;
    if (bake_cache_find(k)) return;   /* already kept, now touched */
    cost = sizeof *e + o->length + sh_nav_map_bytes(&k->shapes);
    while (g_cache_count >= BAKE_CACHE_MAX_ENTRIES || g_cache_bytes + cost > g_cache_budget) {
        int i, oldest = -1;
        for (i = 0; i < g_cache_count; i++)
            if (g_cache[i]->used < g_cache_pass &&
                (oldest < 0 || g_cache[i]->used < g_cache[oldest]->used)) oldest = i;
        if (oldest < 0) break;
        bake_cache_drop(oldest);
        g_cache_evicted++;
    }
    if (g_cache_count >= BAKE_CACHE_MAX_ENTRIES || g_cache_bytes + cost > g_cache_budget) {
        g_cache_declined++;
        if (!g_cache_full_logged) {
            char line[256];
            g_cache_full_logged = 1;
            _snprintf_s(line, sizeof line, _TRUNCATE,
                        "NAV: the bake cache is full (%d payload(s), %.1f of %.0f MiB, all "
                        "in use by this pass); a %.1f MiB bake is not kept and is baked again "
                        "next time", g_cache_count, g_cache_bytes / 1048576.0,
                        g_cache_budget / 1048576.0, o->length / 1048576.0);
            backend_log(line);
        }
        return;
    }
    if (!(g_cache_count & 63)) {
        bake_cache_entry **grown = (bake_cache_entry **)realloc(
            g_cache, (size_t)(g_cache_count + 64) * sizeof *grown);
        if (!grown) return;
        g_cache = grown;
    }
    e = (bake_cache_entry *)calloc(1, sizeof *e);
    if (!e) return;
    e->bytes = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, o->length ? o->length : 1);
    if (!e->bytes) { free(e); return; }
    memcpy(e->bytes, o->bytes, o->length);
    e->key = *k;
    memset(&k->shapes, 0, sizeof k->shapes);
    e->length = o->length;
    e->first_area = o->first_area;
    e->state = o->state;
    strcpy_s(e->reason, sizeof e->reason, o->reason);
    strcpy_s(e->brief, sizeof e->brief, o->brief);
    strcpy_s(e->faces, sizeof e->faces, o->faces);
    e->faces_climbed = o->faces_climbed; e->faces_total = o->faces_total;
    memcpy(e->partial, o->partial, sizeof e->partial);
    e->partial_count = o->partial_count;
    e->cost = cost;
    e->used = ++g_cache_clock;
    g_cache[g_cache_count++] = e;
    g_cache_bytes += cost;
    g_cache_stores++;
}

/* Bake one class inline, for BuildAAS, or serve the same boxes' earlier bake.
 * Inputs are gathered here, on the thread that holds the bake lock; the
 * scratch lives on the heap, not on the loader thread's stack. Sets *reused
 * when the outcome came from the cache. */
static void bake_one(const char *name, const bake_module *m, int ci,
                     sh_nav_bake_reader read_shipped, bake_outcome *o, int *traversal,
                     int *reused)
{
    unsigned char *shipped = NULL;
    size_t shipped_len = 0;
    sh_aug_platform *plats;
    bake_cache_key key;
    int n, left_out, not_floor, keyed = 0;
    *traversal = 0;
    *reused = 0;

    plats = bake_collect_platforms(m, &n, &left_out, &not_floor);
    /* Load traversal animations lazily through the active provider;
     * successful reads are cached for the session. Without a table, no climbs
     * are emitted.
     */
    if (n) *traversal = sh_trav_load(read_shipped);
    if (n) {
        SH_PERF_BEGIN(tr);
        shipped = read_shipped ? read_shipped(name, &shipped_len) : NULL;
        SH_PERF_END(SH_PERF_BAKE_READ, tr);
    }
    if (n && shipped) {
        keyed = bake_cache_key_make(&key, m, ci, shipped, shipped_len, *traversal);
        if (keyed) {
            bake_cache_entry *e = bake_cache_find(&key);
            if (e && bake_cache_serve(e, o)) *reused = 1;
        }
    }
    if (!*reused) {
        bake_run(name, plats, n, shipped, shipped_len, left_out, not_floor, o);
        if (keyed) bake_cache_store(&key, o);
    }
    if (keyed) bake_cache_key_free(&key);
    if (shipped) HeapFree(GetProcessHeap(), 0, shipped);
    if (plats) HeapFree(GetProcessHeap(), 0, plats);
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

/* ==================================================================== */
/* the preview bake, off the calling thread                              */
/* ==================================================================== */

/* Folding the marked volumes into a module's navigation is tens of
 * milliseconds of arithmetic on buffers we own, so it runs on a worker while
 * the frame that asked for it carries on. The two reads that touch the engine
 * -- the traversal table and the module's shipped navigation -- happen on the
 * calling thread, before the hand-off.
 *
 * The cost is that the green trails a change by a frame or two. */

typedef struct preview_task {
    int              slot;                   /* index into g_modules and g_preview */
    int              instance;
    char             name[SH_SMNAV_RESNAME_CAP];
    sh_aug_platform *plats;
    int              plat_count;
    int              left_out, not_floor;
    unsigned char   *shipped;
    size_t           shipped_len;
    int              traversal;
    int              cached;                 /* out came from the bake cache */
    bake_outcome     out;                    /* what the worker produced */
} preview_task;

typedef struct preview_batch {
    unsigned long revision;                  /* the geometry it was gathered from */
    int           count;
    preview_task *t;                         /* count entries, one heap block with the batch */
} preview_batch;

static HANDLE           g_pv_thread, g_pv_wake;
static CRITICAL_SECTION g_pv_lock;
static volatile LONG    g_pv_ready;          /* the lock and the event exist */
static preview_batch   *g_pv_queued;         /* handed over, not started */
static preview_batch   *g_pv_done;           /* finished, not installed */
static unsigned long    g_pv_inflight;       /* revision the worker holds, 0 = idle */
static volatile LONG    g_pv_stop;

static preview_batch *preview_batch_alloc(int count)
{
    preview_batch *b = (preview_batch *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                           sizeof *b + (size_t)(count > 0 ? count : 1) * sizeof *b->t);
    if (b) b->t = (preview_task *)(b + 1);
    return b;
}

static void preview_batch_free(preview_batch *b)
{
    int i;
    if (!b) return;
    for (i = 0; i < b->count; i++) {
        if (b->t[i].plats)     HeapFree(GetProcessHeap(), 0, b->t[i].plats);
        if (b->t[i].shipped)   HeapFree(GetProcessHeap(), 0, b->t[i].shipped);
        if (b->t[i].out.bytes) HeapFree(GetProcessHeap(), 0, b->t[i].out.bytes);
    }
    HeapFree(GetProcessHeap(), 0, b);
}

static void preview_batch_run(preview_batch *b)
{
    int i;
    SH_PERF_BEGIN(t0);
    for (i = 0; i < b->count; i++) {
        preview_task *t = &b->t[i];
        if (t->cached) continue;
        bake_run(t->name, t->plats, t->plat_count, t->shipped, t->shipped_len,
                 t->left_out, t->not_floor, &t->out);
    }
    SH_PERF_END(SH_PERF_PREVIEW_BAKE, t0);
}

static DWORD WINAPI preview_worker(LPVOID unused)
{
    (void)unused;
    for (;;) {
        preview_batch *b = NULL;
        WaitForSingleObject(g_pv_wake, INFINITE);
        if (InterlockedCompareExchange(&g_pv_stop, 0, 0)) return 0;
        EnterCriticalSection(&g_pv_lock);
        b = g_pv_queued;
        g_pv_queued = NULL;
        LeaveCriticalSection(&g_pv_lock);
        if (!b) continue;
        preview_batch_run(b);
        EnterCriticalSection(&g_pv_lock);
        preview_batch_free(g_pv_done);   /* a result nobody collected is stale */
        g_pv_done = b;
        LeaveCriticalSection(&g_pv_lock);
    }
}

/* 1 once a worker exists. A machine that refuses the thread bakes inline, which
 * is what this did before, so the preview still works. */
static int preview_worker_ready(void)
{
    if (InterlockedCompareExchange(&g_pv_ready, 0, 0) == 2) return g_pv_thread != NULL;
    if (InterlockedCompareExchange(&g_pv_ready, 1, 0) != 0) return g_pv_thread != NULL;
    InitializeCriticalSection(&g_pv_lock);
    g_pv_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (g_pv_wake)
        g_pv_thread = CreateThread(NULL, 0, preview_worker, NULL, 0, NULL);
    InterlockedExchange(&g_pv_ready, 2);
    return g_pv_thread != NULL;
}

int sh_nav_bake_show_marks(int count)
{
    int i, marked = 0;
    if (count < 0) count = 0;
    if (count > BAKE_MAX_REFUSED) count = BAKE_MAX_REFUSED;
    AcquireSRWLockExclusive(&g_bake_lock);
    for (i = 0; i < g_module_count; i++) {
        int want = 0, r;
        if (g_preview[i].refused) {
            HeapFree(GetProcessHeap(), 0, g_preview[i].refused);
            g_preview[i].refused = NULL;
        }
        g_preview[i].refused_count = 0;
        g_preview[i].instance = g_modules[i].instance;
        if (count == 0) continue;
        for (r = 0; r < g_map.region_count && want < count; r++)
            if (g_map.regions[r].instance == g_modules[i].instance) want++;
        if (want == 0) continue;
        g_preview[i].refused = (bake_refused *)HeapAlloc(GetProcessHeap(), 0,
                                    (size_t)want * sizeof *g_preview[i].refused);
        if (!g_preview[i].refused) continue;
        want = 0;
        for (r = 0; r < g_map.region_count && want < count; r++) {
            if (g_map.regions[r].instance != g_modules[i].instance) continue;
            memcpy(g_preview[i].refused[want].c, g_map.regions[r].c,
                   sizeof g_preview[i].refused[0].c);
            want++;
        }
        g_preview[i].refused_count = want;
        marked += want;
    }
    /* The line pass runs on a geometry change, so ask for one. */
    g_geometry_revision++;
    g_preview_revision = g_geometry_revision;
    ReleaseSRWLockExclusive(&g_bake_lock);
    return marked;
}

int sh_nav_bake_preview_pending(void)
{
    int pending;
    if (InterlockedCompareExchange(&g_pv_ready, 0, 0) != 2) return 0;
    EnterCriticalSection(&g_pv_lock);
    pending = g_pv_done != NULL;
    LeaveCriticalSection(&g_pv_lock);
    return pending;
}

void sh_nav_bake_preview_stop(void)
{
    if (InterlockedCompareExchange(&g_pv_ready, 0, 0) != 2) return;
    InterlockedExchange(&g_pv_stop, 1);
    if (g_pv_wake) SetEvent(g_pv_wake);
    if (g_pv_thread) WaitForSingleObject(g_pv_thread, 2000);
}

/* Everything the bake needs, read on this thread while the lock is held. */
static preview_batch *preview_gather(sh_nav_bake_reader read_shipped)
{
    preview_batch *b;
    int i, traversal;

    b = preview_batch_alloc(g_module_count);
    if (!b) return NULL;
    b->revision = g_geometry_revision;
    bake_cache_begin_pass();

    /* Cached until its sources are invalidated; without it no climbs are emitted.
     * It reads effective resources, so it cannot go to the worker. */
    traversal = sh_trav_load(read_shipped);

    for (i = 0; i < g_module_count; i++) {
        preview_task *t;
        const char *base;
        if (!g_modules[i].ok) continue;
        base = strrchr(g_modules[i].module, '/');
        if (!base) continue;
        t = &b->t[b->count++];
        t->slot = i;
        t->instance = g_modules[i].instance;
        t->traversal = traversal;
        _snprintf_s(t->name, sizeof t->name, _TRUNCATE,
                    "generated/maps/modules/%s/%s.baas_monster48",
                    g_modules[i].module, base + 1);
        t->plats = bake_collect_platforms(&g_modules[i], &t->plat_count,
                                          &t->left_out, &t->not_floor);
        if (t->plat_count > 0 && !t->plats) { b->count--; continue; }
        if (t->plat_count > 0) {
            SH_PERF_BEGIN(tr);
            t->shipped = read_shipped ? read_shipped(t->name, &t->shipped_len) : NULL;
            SH_PERF_END(SH_PERF_BAKE_READ, tr);
        }
        /* A copy whose boxes were baked before needs no worker time: an edit
         * to one copy rebuilds that copy's preview only. */
        if (t->plat_count > 0 && t->shipped) {
            bake_cache_key key;
            if (bake_cache_key_make(&key, &g_modules[i], 0, t->shipped, t->shipped_len,
                                    traversal)) {
                bake_cache_entry *e = bake_cache_find(&key);
                if (e && bake_cache_serve(e, &t->out)) t->cached = 1;
            }
            bake_cache_key_free(&key);
        }
    }
    return b;
}

static bake_refused *bake_copy_marks(const bake_refused *src, int count)
{
    bake_refused *out;
    if (count <= 0) return NULL;
    out = (bake_refused *)HeapAlloc(GetProcessHeap(), 0, (size_t)count * sizeof *out);
    if (out) memcpy(out, src, (size_t)count * sizeof *out);
    return out;
}

static void bake_class_set(bake_class *k, const bake_outcome *o)
{
    k->state = o->state;
    k->revision = g_geometry_revision;
    k->logged = 0;
    strncpy_s(k->reason, sizeof k->reason, o->reason, _TRUNCATE);
    strncpy_s(k->brief, sizeof k->brief, o->brief, _TRUNCATE);
    strncpy_s(k->faces, sizeof k->faces, o->faces, _TRUNCATE);
    k->faces_climbed = o->faces_climbed; k->faces_total = o->faces_total;
}

/* Move a finished batch into the preview cache. Called with the bake lock
 * held, and only for the geometry it was gathered from -- a batch overtaken by
 * a later edit describes a map that no longer exists. */
static void preview_install(preview_batch *b)
{
    int i, served = 0;
    if (b->revision != g_geometry_revision) return;
    for (i = 0; i < b->count; i++) {
        preview_task *t = &b->t[i];
        bake_preview_cache *pc;
        if (t->slot < 0 || t->slot >= g_module_count) continue;
        if (t->cached) served++;
        else if (t->out.bytes && t->shipped) {
            /* The geometry is the one gathered (same revision), so the key is too. */
            bake_cache_key key;
            if (bake_cache_key_make(&key, &g_modules[t->slot], 0, t->shipped,
                                    t->shipped_len, t->traversal))
                bake_cache_store(&key, &t->out);
            bake_cache_key_free(&key);
        }
        pc = &g_preview[t->slot];
        if (pc->bytes) HeapFree(GetProcessHeap(), 0, pc->bytes);
        if (pc->refused) HeapFree(GetProcessHeap(), 0, pc->refused);
        if (pc->partial) HeapFree(GetProcessHeap(), 0, pc->partial);
        pc->bytes = t->out.bytes;
        pc->length = t->out.length;
        pc->first_area = t->out.first_area;
        pc->instance = t->instance;
        strcpy_s(pc->source, sizeof(pc->source), t->name);
        pc->traversal = t->traversal;
        pc->done = 1;
        pc->state = t->out.bytes ? t->out.state : SH_NAV_CLASS_REFUSED;
        t->out.bytes = NULL;   /* the cache owns it now */
        pc->refused = bake_copy_marks(t->out.refused, t->out.refused_count);
        pc->refused_count = pc->refused ? t->out.refused_count : 0;
        pc->partial = bake_copy_marks(t->out.partial, t->out.partial_count);
        pc->partial_count = pc->partial ? t->out.partial_count : 0;
        bake_class_set(&g_modules[t->slot].cls[0], &t->out);
        g_modules[t->slot].cls[0].state = pc->state;
        strncpy_s(g_modules[t->slot].reason, sizeof g_modules[t->slot].reason,
                  t->out.reason, _TRUNCATE);
        if (!pc->bytes) {
            char message[SH_SMNAV_RESNAME_CAP + BAKE_REASON_CAP + 96];
            _snprintf_s(message, sizeof message, _TRUNCATE,
                        "NAV: preview refused for copy %d '%s' -- %s",
                        t->instance, t->name, t->out.reason);
            backend_log(message);
        } else if (pc->state == SH_NAV_CLASS_PARTIAL) {
            char message[SH_SMNAV_RESNAME_CAP + BAKE_REASON_CAP + 96];
            _snprintf_s(message, sizeof message, _TRUNCATE,
                        "NAV: preview built copy %d '%s' with parts left out -- %s",
                        t->instance, t->name, t->out.brief);
            backend_log(message);
        }
        if (pc->bytes && t->out.faces[0] && !t->cached) {
            char message[SH_SMNAV_RESNAME_CAP + BAKE_FACES_CAP + 96];
            _snprintf_s(message, sizeof message, _TRUNCATE,
                        "NAV: preview floor climbs for copy %d, %d of %d box face(s): %s",
                        t->instance, t->out.faces_climbed, t->out.faces_total, t->out.faces);
            backend_log(message);
        }
    }
    if (served) {
        char message[128];
        _snprintf_s(message, sizeof message, _TRUNCATE,
                    "NAV: preview reused %d of %d copy bake(s) whose boxes are unchanged",
                    served, b->count);
        backend_log(message);
    }
    g_preview_revision = b->revision;
}

/* The worker cache is also consumed by Play. Building it must not depend on
 * whether the mapper has enabled the green preview or opened Object mode. */
static int preview_prepare_locked(sh_nav_bake_reader read_shipped)
{
    if(!g_have_map||g_live_refused) return -1;
    if (!g_module_count) {
        g_preview_revision = g_geometry_revision;
        return 1;
    }

    /* Collect whatever the worker finished, then schedule the next bake if
     * the geometry has moved past what is on screen. */
    if (InterlockedCompareExchange(&g_pv_ready, 0, 0) == 2) {
        preview_batch *done = NULL;
        EnterCriticalSection(&g_pv_lock);
        done = g_pv_done; g_pv_done = NULL;
        LeaveCriticalSection(&g_pv_lock);
        if (done) {
            if (done->revision == g_pv_inflight) g_pv_inflight = 0;
            preview_install(done);
            preview_batch_free(done);
        }
    }
    if (g_preview_revision != g_geometry_revision &&
        g_pv_inflight != g_geometry_revision) {
        preview_batch *b = preview_gather(read_shipped);
        if (!b) return -1;
        if (preview_worker_ready()) {
            EnterCriticalSection(&g_pv_lock);
            preview_batch_free(g_pv_queued);   /* an older batch is superseded */
            g_pv_queued = b;
            LeaveCriticalSection(&g_pv_lock);
            g_pv_inflight = b->revision;
            SetEvent(g_pv_wake);
        } else {
            preview_batch_run(b);
            preview_install(b);
            preview_batch_free(b);
        }
    }
    return g_preview_revision == g_geometry_revision;
}

int sh_nav_bake_prepare(sh_nav_bake_reader read_shipped)
{
    int current = -1;
    if (!read_shipped || !bake_enabled()) return -1;
    if (InterlockedCompareExchange(&g_building, 0, 0)) return 0;
    AcquireSRWLockExclusive(&g_bake_lock);
    __try { current = preview_prepare_locked(read_shipped); }
    __finally { ReleaseSRWLockExclusive(&g_bake_lock); }
    return current;
}

static void bake_preview_marks(const bake_refused *marks, int count, int inst,
                               float r, float g, float b, sh_nav_preview_line line,
                               sh_nav_preview_colour_fn colour, void *ctx)
{
    int k;
    if (!colour || count <= 0 || inst < 0 || inst >= g_map.instance_count) return;
    colour(r, g, b, ctx);
    for (k = 0; k < count; k++)
        bake_preview_polygon(&g_map.instances[inst], marks[k].c, 4, line, ctx);
    colour(0.15f, 1.0f, 0.25f, ctx);
}

int sh_nav_bake_preview(sh_nav_bake_reader read_shipped,sh_nav_preview_line line,
                        sh_nav_preview_colour_fn colour,void *ctx)
{
    int i, current = 0;
    if(!line||!read_shipped||!bake_enabled()||InterlockedCompareExchange(&g_building,0,0))return 0;
    AcquireSRWLockExclusive(&g_bake_lock);
    __try {
        int prepared = preview_prepare_locked(read_shipped);
        if (prepared < 0) { current = 1; __leave; }
        /* Still waiting on the worker: the lines that are up describe the
         * previous geometry, which is closer to the truth than none. */
        if (!prepared) __leave;
        current = 1;
        SH_PERF_BEGIN(tl);
        g_preview_lines=0;

        /* Volumes the bake refused, marked red where they stand, and volumes
         * that lost routes or faces so the rest fits, in amber. Drawn first so
         * a mark is never hidden under the walkable surface beside it. */
        for(i=0;i<g_module_count;i++) {
            bake_preview_marks(g_preview[i].refused,g_preview[i].refused_count,
                               g_preview[i].instance,1.0f,0.15f,0.15f,line,colour,ctx);
            bake_preview_marks(g_preview[i].partial,g_preview[i].partial_count,
                               g_preview[i].instance,1.0f,0.65f,0.1f,line,colour,ctx);
        }

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
        SH_PERF_END(SH_PERF_PREVIEW_LINES, tl);

        /* A preview that empties with volumes still marked is the one
         * failure a mapper sees and the log never recorded. */
        if (g_preview_lines == 0 && g_preview_lines_last > 0) {
            char message[160];
            int k, payloads = 0, marks = 0;
            for (k = 0; k < g_module_count; k++) {
                if (g_preview[k].bytes) payloads++;
                marks += g_preview[k].refused_count;
            }
            _snprintf_s(message, sizeof message, _TRUNCATE,
                        "NAV: the green preview is now empty -- %d module(s) marked, "
                        "%d with a finished bake, %d volume(s) marked as not placed",
                        g_module_count, payloads, marks);
            backend_log(message);
        } else if (g_preview_lines > 0 && g_preview_lines_last == 0) {
            char message[96];
            _snprintf_s(message, sizeof message, _TRUNCATE,
                        "NAV: the green preview is back -- %d line(s)", g_preview_lines);
            backend_log(message);
        }
        g_preview_lines_last = g_preview_lines;
    } __finally { ReleaseSRWLockExclusive(&g_bake_lock); }
    return current;
}

int sh_nav_bake_open(const char *name, sh_nav_bake_reader read_shipped,
                     unsigned char **out_bytes, size_t *out_len)
{
    char module[SH_NAVR_MODULE_CAP], cls[32];
    char why[BAKE_REASON_CAP], skipped[BAKE_REASON_CAP], faces[BAKE_FACES_CAP + 48] = "";
    bake_module *m;
    int hit = 0, i, ci, instance=-1, attempted=0, partial=0, reused=0;
    unsigned long revision=0;
    char canonical[384], source[SH_SMNAV_RESNAME_CAP];
    int traversal = 0;

    if (!name || !out_bytes || !out_len) return 0;
    *out_bytes = NULL;
    *out_len = 0;
    skipped[0] = 0;
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
    ci = bake_class_index(cls);
    if (ci < 0) return 0;
    if (strcpy_s(source, sizeof(source), name)) return 0;
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
        int slot=(int)(m-g_modules);
        bake_class *k=&m->cls[ci];
        why[0] = 0;
        if (ci == 0 && g_preview_revision == g_geometry_revision && g_preview[slot].bytes) {
            /* The worker already baked exactly this. */
            attempted = 1;
            *out_bytes=(unsigned char*)HeapAlloc(GetProcessHeap(),0,g_preview[slot].length);
            if(*out_bytes){memcpy(*out_bytes,g_preview[slot].bytes,g_preview[slot].length);
                *out_len=g_preview[slot].length;hit=1;
                strcpy_s(source, sizeof(source), g_preview[slot].source);
                traversal = g_preview[slot].traversal;reused=2;}
            strncpy_s(why,sizeof why,k->reason[0]?k->reason:m->reason,_TRUNCATE);
            partial = g_preview[slot].state == SH_NAV_CLASS_PARTIAL;
        } else if (bake_class_refused_locked(m, ci)) {
            /* The same inputs refuse the same way: keep the reason, skip the
             * bake, and let the engine load the shipped navigation. The other
             * classes of this copy are unaffected. */
            if (k != &m->cls[0] &&
                (k->revision != g_geometry_revision || k->state != SH_NAV_CLASS_REFUSED)) {
                k->state = SH_NAV_CLASS_REFUSED;
                k->revision = g_geometry_revision;
                strncpy_s(k->reason, sizeof k->reason, m->reason, _TRUNCATE);
                strncpy_s(k->brief, sizeof k->brief, m->cls[0].brief, _TRUNCATE);
            }
            if (!k->logged) {
                k->logged = 1;
                strncpy_s(skipped, sizeof skipped, k->reason, _TRUNCATE);
            }
        } else {
            bake_outcome *o = (bake_outcome *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *o);
            attempted = 1;
            __try {
                if (!o) {
                    strcpy_s(why, sizeof why, "out of memory");
                } else {
                    bake_one(name, m, ci, read_shipped, o, &traversal, &reused);
                    bake_class_set(k, o);
                    strncpy_s(why, sizeof why, o->reason, _TRUNCATE);
                    _snprintf_s(faces, sizeof faces, _TRUNCATE, "%d of %d box face(s): %s",
                                o->faces_climbed, o->faces_total, o->faces);
                    if (o->bytes) {
                        *out_bytes = o->bytes; *out_len = o->length; o->bytes = NULL;
                        hit = 1;
                        partial = o->state == SH_NAV_CLASS_PARTIAL;
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                /* Disable baking after a fault to avoid repeating it during map load. */
                InterlockedExchange(&g_faulted, 1);
                hit = 0;
                _snprintf_s(why, sizeof why, _TRUNCATE, "the bake faulted; navigation is off for this session");
                k->state = SH_NAV_CLASS_REFUSED; k->revision = g_geometry_revision;
                strncpy_s(k->reason, sizeof k->reason, why, _TRUNCATE);
                strcpy_s(k->brief, sizeof k->brief, "the bake faulted");
            }
            if (o) { if (o->bytes) HeapFree(GetProcessHeap(), 0, o->bytes); HeapFree(GetProcessHeap(), 0, o); }
        }
        if (hit) InterlockedIncrement(&g_bakes);
    }
    ReleaseSRWLockExclusive(&g_bake_lock);

    if (hit) {
        char line[SH_SMNAV_RESNAME_CAP + BAKE_REASON_CAP + 64];
        /* A served bake says so: "baked" is a bake that ran during this load. */
        _snprintf_s(line, sizeof line, _TRUNCATE, "NAV: %s '%s'%s%s -- %s",
                    reused ? "reused" : "baked", name,
                    partial ? " with parts left out" : "",
                    reused == 2 ? " from the preview" :
                    reused ? " from an earlier bake of the same boxes" : "", why);
        backend_log(line);
        if (strstr(faces, ": ") && faces[strlen(faces) - 1] != ' ') {
            char line2[SH_SMNAV_RESNAME_CAP + BAKE_FACES_CAP + 64];
            _snprintf_s(line2, sizeof line2, _TRUNCATE, "NAV:   floor climbs in '%s', %s", name, faces);
            backend_log(line2);
        }
    } else if(attempted) {
        char line[SH_SMNAV_RESNAME_CAP+BAKE_REASON_CAP+96];
        _snprintf_s(line,sizeof line,_TRUNCATE,"NAV: bake refused for copy %d '%s' -- %s",
            instance,name,why);
        backend_log(line);
    } else if (skipped[0]) {
        char line[SH_SMNAV_RESNAME_CAP+BAKE_REASON_CAP+96];
        _snprintf_s(line,sizeof line,_TRUNCATE,
            "NAV: kept the shipped navigation for copy %d '%s' -- refused earlier: %s",
            instance,name,skipped);
        backend_log(line);
    }
    if(!hit&&instance>=0&&read_shipped) {
        traversal = 0;
        *out_bytes=read_shipped(name,out_len);
        hit=*out_bytes!=NULL;
    }
    return hit;
}

/* The leaf of "category/module", which is what a mapper recognizes. */
static const char *bake_leaf(const char *module)
{
    const char *slash = strrchr(module, '/');
    return slash ? slash + 1 : module;
}

void sh_nav_bake_summary_get(sh_nav_bake_summary *s)
{
    int i, first_refused = -1, first_partial = -1;
    if (!s) return;
    memset(s, 0, sizeof *s);
    AcquireSRWLockShared(&g_bake_lock);
    s->have_map = bake_map_held_locked();
    strncpy_s(s->read_reason, sizeof s->read_reason, g_read_reason, _TRUNCATE);
    if (s->have_map) {
        s->boxes_left_out = g_map.dropped_total;
        for (i = 0; i < g_module_count; i++) {
            const bake_preview_cache *pc = &g_preview[i];
            if (!g_modules[i].ok) continue;
            s->rooms++;
            if (g_preview_revision != g_geometry_revision || !pc->done) { s->pending++; continue; }
            s->faces_climbed += g_modules[i].cls[0].faces_climbed;
            s->faces_total += g_modules[i].cls[0].faces_total;
            if (!pc->bytes) { s->refused++; if (first_refused < 0) first_refused = i; }
            else if (pc->state == SH_NAV_CLASS_PARTIAL) {
                s->partial++; if (first_partial < 0) first_partial = i;
            } else s->built++;
        }
        i = first_refused >= 0 ? first_refused : first_partial;
        if (i >= 0)
            _snprintf_s(s->first, sizeof s->first, _TRUNCATE, "%s (copy %d): %s",
                        bake_leaf(g_modules[i].module), g_modules[i].instance,
                        g_modules[i].cls[0].brief[0] ? g_modules[i].cls[0].brief
                                                    : "see sh_navmesh");
        else if (g_map.dropped_kept > 0)
            _snprintf_s(s->first, sizeof s->first, _TRUNCATE, "box %u: %s",
                        g_map.dropped[0].entity,
                        sh_nav_regions_drop_reason(g_map.dropped[0].reason));
    }
    ReleaseSRWLockShared(&g_bake_lock);
}

/* Fit "<head><tail>" into cap, shortening only the middle detail. */
static void bake_compose(char *out, size_t cap, const char *head, const char *detail,
                         const char *tail)
{
    size_t room = cap, fixed = strlen(head) + strlen(tail) + 1, take;
    char cut[SH_NAV_TOAST_BODY_CAP];
    if (!out || !cap) return;
    take = fixed < room ? room - fixed : 0;
    if (take >= sizeof cut) take = sizeof cut - 1;
    strncpy_s(cut, sizeof cut, detail ? detail : "", _TRUNCATE);
    if (strlen(cut) > take) {
        if (take >= 3) { cut[take - 3] = 0; strcat_s(cut, sizeof cut, "..."); }
        else cut[take] = 0;
    }
    _snprintf_s(out, cap, _TRUNCATE, "%s%s%s", head, cut, tail);
}

void sh_nav_bake_toast(const sh_nav_bake_summary *s, int finished, char *title,
                       size_t title_cap, char *body, size_t body_cap)
{
    char head[96], tail[64];
    size_t cap = body_cap < SH_NAV_TOAST_BODY_CAP ? body_cap : SH_NAV_TOAST_BODY_CAP;
    if (!title || !title_cap || !body || !body_cap) return;
    if (!finished || !s || !s->have_map) {
        strcpy_s(title, title_cap, "Navmesh Build Failed");
        if (s && !s->have_map && s->read_reason[0])
            bake_compose(body, cap, "This map could not be read for navigation: ",
                         s->read_reason, ". Run sh_navmesh for details.");
        else
            strcpy_s(body, cap, "The navmesh could not be completed. Check the navigation "
                                "settings and marked geometry, then try again.");
        return;
    }
    if (!s->refused && !s->partial && !s->boxes_left_out) {
        strcpy_s(title, title_cap, "Navmesh Built");
        if (s->faces_total > s->faces_climbed)
            _snprintf_s(body, cap, _TRUNCATE, "The map's navmesh is ready. Demons can climb "
                        "%d of %d box faces from the floor; sh_navmesh lists them.",
                        s->faces_climbed, s->faces_total);
        else strcpy_s(body, cap, "The map's navmesh is ready.");
        return;
    }
    if (!s->built && !s->partial) {
        strcpy_s(title, title_cap, "Navmesh Build Failed");
        strcpy_s(head, sizeof head, s->rooms ? "No room could be built. "
                                             : "No marked box could be used. ");
    } else {
        strcpy_s(title, title_cap, "Navmesh Partly Built");
        _snprintf_s(head, sizeof head, _TRUNCATE, "%d of %d rooms built%s. ",
                    s->built + s->partial, s->rooms,
                    s->partial ? (s->refused ? ", some partly" : ", partly") : "");
    }
    if (s->boxes_left_out)
        _snprintf_s(tail, sizeof tail, _TRUNCATE, ". %d box%s left out. See sh_navmesh.",
                    s->boxes_left_out, s->boxes_left_out == 1 ? "" : "es");
    else strcpy_s(tail, sizeof tail, ". See sh_navmesh.");
    bake_compose(body, cap, head, s->first[0] ? s->first : "some rooms kept their own navmesh",
                 tail);
}

static const char *bake_class_text(int state)
{
    switch (state) {
    case SH_NAV_CLASS_BUILT:   return "built";
    case SH_NAV_CLASS_PARTIAL: return "built with parts left out";
    case SH_NAV_CLASS_REFUSED: return "keeps its shipped navigation";
    }
    return "not baked yet";
}

void sh_nav_bake_report(void (*out)(const char *fmt, ...))
{
    int i, c;
    if (!out) return;
    if (InterlockedCompareExchange(&g_faulted, 0, 0) != 0)
        out("navigation: baking is OFF for this session after a fault.\n");
    AcquireSRWLockShared(&g_bake_lock);
    if (g_module_count == 0 && !g_live_refused) {
        out("navigation: no volume in this map is marked for AI navigation.\n");
    } else {
        for (i = 0; i < g_module_count; i++) {
            const bake_module *m = &g_modules[i];
            out("  marked %s -- %d volume(s) in copy %d%s\n",
                m->module, m->regions, m->instance, m->ok ? "" : " -- not baked");
            if (!m->ok) { out("    %s\n", m->reason); continue; }
            if (m->not_floor)
                out("    %d marked box(es) do not block demons and are not floors\n", m->not_floor);
            for (c = 0; c < NAV_CLASS_COUNT; c++) {
                const bake_class *k = &m->cls[c];
                int state = k->revision == g_geometry_revision ? k->state : SH_NAV_CLASS_UNTRIED;
                if (c == 0 && g_preview_revision == g_geometry_revision && g_preview[i].done)
                    state = g_preview[i].state;
                out("    %s: %s%s%s\n", NAV_CLASSES[c], bake_class_text(state),
                    state != SH_NAV_CLASS_UNTRIED && k->reason[0] ? " -- " : "",
                    state != SH_NAV_CLASS_UNTRIED ? k->reason
                        : (c ? " (baked when Play loads it)" : ""));
                if (state != SH_NAV_CLASS_UNTRIED && k->faces[0])
                    out("      floor climbs on %d of %d box face(s): %s\n",
                        k->faces_climbed, k->faces_total, k->faces);
            }
            out("    preview: %s, %d volume(s) marked as not placed, %d drawn amber\n",
                g_preview[i].bytes ? "drawn from a finished bake" : "nothing to draw",
                g_preview[i].refused_count, g_preview[i].partial_count);
        }
    }
    if (bake_map_held_locked() && g_map.dropped_total) {
        out("  %d box(es) left out of the map read; the rest of the map kept its navigation:\n",
            g_map.dropped_total);
        for (i = 0; i < g_map.dropped_kept; i++) {
            const sh_nav_dropped *d = &g_map.dropped[i];
            out("    %s box %u (id %d)%s: %s\n", d->marked ? "marked" : "Block Demons",
                d->entity, d->uid, d->instance >= 0 ? " in a room" : "",
                sh_nav_regions_drop_reason(d->reason));
        }
        if (g_map.dropped_total > g_map.dropped_kept)
            out("    ... and %d more\n", g_map.dropped_total - g_map.dropped_kept);
    }
    if (g_live_refused) {
        out("  current editor geometry could not be read (%s); custom navigation is "
            "unavailable until a complete snapshot succeeds.\n",
            g_read_reason[0] ? g_read_reason : "no reason recorded");
    } else if (g_live_scanned < 0) {
        out("  using the loaded map; a complete editor snapshot is pending.\n");
    } else {
        out("  the current editor snapshot contains %d marked volume(s).\n",g_live_marked);
    }
    out("  editor geometry revision %lu; green preview: monster48, %d lines%s.\n",
        g_geometry_revision,g_preview_lines,g_preview_lines>=8192?" (display limit reached)":"");
    out("  map tables: %d room(s), %d floor(s), %d wall(s), %lu KB\n",
        g_map.instance_count, g_map.region_count, g_map.obstacle_count,
        (unsigned long)(sh_nav_map_bytes(&g_map) / 1024));
    for (i = 0; i < g_census_count; i++)
        out("  the engine opened %s navigation %u time(s)\n",
            g_census[i].module, g_census[i].opens);
    out("  bake cache: %d payload(s), %.1f of %.0f MiB; %lu served, %lu kept, %lu evicted, "
        "%lu not kept for want of room.\n", g_cache_count, g_cache_bytes / 1048576.0,
        g_cache_budget / 1048576.0, g_cache_hits, g_cache_stores, g_cache_evicted,
        g_cache_declined);
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
    bake_cache_clear();
    g_cache_budget = BAKE_CACHE_BUDGET;
    g_cache_hits = g_cache_stores = g_cache_evicted = g_cache_declined = 0;
    g_instance_serving=0;
    sh_nav_map_free(&g_map);
    if (g_modules) memset(g_modules, 0, (size_t)g_module_cap * sizeof *g_modules);
    g_module_count = 0;
    g_have_map = 0;
    g_snapshot = NULL; g_snapshot_ctx = NULL;
    g_building = 0; g_geometry_revision = 0;
    g_read_reason = "";
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
    sh_nav_map_copy(out, &g_map);
    ReleaseSRWLockShared(&g_bake_lock);
}

void sh_nav_bake_test_preview_inputs(unsigned long revision, const char *bytes, const char *source, int traversal)
{
    preview_batch *batch = preview_batch_alloc(1);
    size_t length = strlen(bytes);
    if (!batch) return;
    batch->revision = revision; batch->count = 1;
    strcpy_s(batch->t[0].name, sizeof(batch->t[0].name), source);
    batch->t[0].traversal = traversal;
    batch->t[0].out.state = SH_NAV_CLASS_BUILT;
    batch->t[0].out.bytes = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, length);
    if (!batch->t[0].out.bytes) { preview_batch_free(batch); return; }
    memcpy(batch->t[0].out.bytes, bytes, length); batch->t[0].out.length = length;
    AcquireSRWLockExclusive(&g_bake_lock);
    batch->t[0].instance = g_module_count ? g_modules[0].instance : 0;
    preview_install(batch);
    ReleaseSRWLockExclusive(&g_bake_lock);
    preview_batch_free(batch);
}

void sh_nav_bake_test_preview_complete(unsigned long revision, const char *bytes)
{
    char source[SH_SMNAV_RESNAME_CAP];
    const char *base;
    AcquireSRWLockShared(&g_bake_lock);
    base = g_module_count ? strrchr(g_modules[0].module, '/') : NULL;
    snprintf(source, sizeof(source), "generated/maps/modules/%s/%s.baas_monster48",
        g_module_count ? g_modules[0].module : "", base ? base + 1 : "");
    ReleaseSRWLockShared(&g_bake_lock);
    sh_nav_bake_test_preview_inputs(revision, bytes, source, 0);
}

/* Run one class bake on given inputs, as the worker and BuildAAS do. */
static char   g_test_reason[BAKE_REASON_CAP];
static size_t g_test_length;
static char   g_test_faces[BAKE_FACES_CAP];
const char *sh_nav_bake_test_last_reason(void) { return g_test_reason; }
const char *sh_nav_bake_test_last_faces(void) { return g_test_faces; }
size_t sh_nav_bake_test_last_length(void) { return g_test_length; }

int sh_nav_bake_test_run(const char *name, const sh_aug_platform *plats, int n,
                         const unsigned char *shipped, size_t shipped_len,
                         char *brief, size_t brief_cap, int *partial_marks)
{
    bake_outcome *o = (bake_outcome *)calloc(1, sizeof *o);
    int state;
    if (!o) return -1;
    bake_run(name, plats, n, shipped, shipped_len, 0, 0, o);
    state = o->state;
    strncpy_s(g_test_reason, sizeof g_test_reason, o->reason, _TRUNCATE);
    strncpy_s(g_test_faces, sizeof g_test_faces, o->faces, _TRUNCATE);
    g_test_length = o->length;
    if (brief && brief_cap) strncpy_s(brief, brief_cap, o->brief, _TRUNCATE);
    if (partial_marks) *partial_marks = o->partial_count;
    if (o->bytes) HeapFree(GetProcessHeap(), 0, o->bytes);
    free(o);
    return state;
}

/* Record a refusal for one class of copy 0, as a bake at this revision would. */
void sh_nav_bake_test_refuse_class(int ci, const char *reason)
{
    AcquireSRWLockExclusive(&g_bake_lock);
    if (g_module_count && ci >= 0 && ci < NAV_CLASS_COUNT) {
        bake_class *k = &g_modules[0].cls[ci];
        k->state = SH_NAV_CLASS_REFUSED;
        k->revision = g_geometry_revision;
        strncpy_s(k->reason, sizeof k->reason, reason, _TRUNCATE);
        strncpy_s(k->brief, sizeof k->brief, reason, _TRUNCATE);
    }
    ReleaseSRWLockExclusive(&g_bake_lock);
}

size_t sh_nav_bake_test_set_cache_budget(size_t bytes)
{
    size_t was;
    AcquireSRWLockExclusive(&g_bake_lock);
    was = g_cache_budget;
    g_cache_budget = bytes ? bytes : BAKE_CACHE_BUDGET;
    ReleaseSRWLockExclusive(&g_bake_lock);
    return was;
}

void sh_nav_bake_test_cache_stats(int *entries, size_t *bytes, unsigned long *served,
                                  unsigned long *evicted, unsigned long *declined)
{
    AcquireSRWLockShared(&g_bake_lock);
    if (entries) *entries = g_cache_count;
    if (bytes) *bytes = g_cache_bytes;
    if (served) *served = g_cache_hits;
    if (evicted) *evicted = g_cache_evicted;
    if (declined) *declined = g_cache_declined;
    ReleaseSRWLockShared(&g_bake_lock);
}

/* Install a worker result for copy `slot` with a chosen outcome. */
void sh_nav_bake_test_preview_outcome(int slot, int state, const char *brief)
{
    preview_batch *batch = preview_batch_alloc(1);
    if (!batch) return;
    AcquireSRWLockExclusive(&g_bake_lock);
    if (slot >= 0 && slot < g_module_count) {
        batch->revision = g_geometry_revision; batch->count = 1;
        batch->t[0].slot = slot;
        batch->t[0].instance = g_modules[slot].instance;
        strcpy_s(batch->t[0].name, sizeof batch->t[0].name, "test");
        batch->t[0].out.state = state;
        strncpy_s(batch->t[0].out.brief, sizeof batch->t[0].out.brief, brief, _TRUNCATE);
        strncpy_s(batch->t[0].out.reason, sizeof batch->t[0].out.reason, brief, _TRUNCATE);
        if (state != SH_NAV_CLASS_REFUSED) {
            batch->t[0].out.bytes = (unsigned char *)HeapAlloc(GetProcessHeap(), 0, 4);
            if (batch->t[0].out.bytes) { memcpy(batch->t[0].out.bytes, "AAS!", 4); batch->t[0].out.length = 4; }
        }
        preview_install(batch);
    }
    ReleaseSRWLockExclusive(&g_bake_lock);
    preview_batch_free(batch);
}
#endif
