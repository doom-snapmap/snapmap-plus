/* nav_bake.c -- see nav_bake.h for what this is and why it bakes at load. */
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

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

#define BAKE_REASON_CAP 192

typedef struct bake_module {
    char module[SH_NAVR_MODULE_CAP];    /* "category/module" */
    int  instance;                      /* the one instance carrying regions */
    int  regions;
    int  ok;
    char reason[BAKE_REASON_CAP];
} bake_module;

#define BAKE_MAX_MODULES 64

static SRWLOCK      g_bake_lock = SRWLOCK_INIT;
static sh_nav_map   g_map;
static bake_module  g_modules[BAKE_MAX_MODULES];
static int          g_module_count;
static int          g_have_map;
static volatile LONG g_bakes;
static volatile LONG g_faulted;

static void bake_plan_locked(void);

/* The live-editor surface, registered once at startup. */
static sh_nav_bake_entity_count g_live_count;
static sh_navr_entity_valid     g_live_valid;
static sh_navr_entity_json      g_live_json;
static void                    *g_live_ctx;
static int                      g_live_refreshed;   /* once per map load */

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
    if (n <= 0) return;
    before = g_module_count;
    marked = sh_nav_regions_refresh_live(&g_map, n, g_live_valid, g_live_json, g_live_ctx);
    if (marked < 0) return;             /* map untouched; keep what the load gave us */
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

/* Decide, once per map, which instance of each module owns the regions -- and
 * refuse a module whose regions are spread over more than one instance. See the
 * header: that is the rule that stays correct whichever way BuildAAS behaves. */
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

        m = bake_find(inst->module);
        if (!m) {
            if (g_module_count >= BAKE_MAX_MODULES) continue;
            m = &g_modules[g_module_count++];
            memset(m, 0, sizeof *m);
            strncpy_s(m->module, sizeof m->module, inst->module, _TRUNCATE);
            m->instance = r->instance;
            m->ok = 1;
        }
        if (m->instance != r->instance) {
            m->ok = 0;
            bake_reason(m,
                "this map marks volumes in more than one copy of the module, and "
                "every copy is built from the same navigation, so they cannot be "
                "given different areas -- mark volumes in only one copy");
            continue;
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

void sh_nav_bake_set_map(const char *json, size_t len)
{
    int ok;
    int i, total = 0, refused = 0;
    char line[256];

    AcquireSRWLockExclusive(&g_bake_lock);
    memset(&g_map, 0, sizeof g_map);
    memset(g_modules, 0, sizeof g_modules);
    g_module_count = 0;
    g_have_map = 0;
    g_live_refreshed = 0;

    if (bake_enabled() && json && len) {
        __try {
            ok = sh_nav_regions_read(json, len, &g_map);
            if (ok) {
                bake_plan_locked();
                g_have_map = 1;
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
        out[n].x0 = r->x0; out[n].y0 = r->y0;
        out[n].x1 = r->x1; out[n].y1 = r->y1;
        out[n].z = r->top_z;
        _snprintf_s(out[n].name, sizeof out[n].name, _TRUNCATE, "volume %u", r->entity);
        n++;
    }
    return n;
}

static int bake_one(const char *name, const bake_module *m, sh_nav_bake_reader read_shipped,
                    unsigned char **out_bytes, size_t *out_len, char *why, size_t why_cap)
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
    opts.fall = SH_AUG_FALL_AUTO;
    opts.inset = 1;
    opts.traversal = 0;

    if (sh_aas_augment(model, plats, n, &opts, &rep)) {
        baked = sh_aas_write(model, &baked_len);
    }
    sh_aas_free(model);
    if (!baked) {
        _snprintf_s(why, why_cap, _TRUNCATE, "the bake did not produce a payload");
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
        int islands = 0, climbs = 0, i;
        for (i = 0; i < rep.platform_count; i++) {
            if (!rep.platforms[i].emitted) continue;
            if (rep.platforms[i].island) islands++;
            climbs += rep.platforms[i].climbs;
        }
        _snprintf_s(why, why_cap, _TRUNCATE,
                    "%d platform(s), areas %u->%u, links %u->%u (%d climb%s), "
                    "tree depth %u->%u%s%s",
                    n, rep.areas_before, rep.areas_after,
                    rep.reach_before, rep.reach_after,
                    climbs, climbs == 1 ? "" : "s",
                    rep.depth_before, rep.depth_after,
                    islands ? "; islands: " : "",
                    islands ? (sh_trav_ready()
                               ? "nothing can climb that high"
                               : "no traversal table, so nothing climbs") : "");
    }
    return rc;
}

int sh_nav_bake_open(const char *name, sh_nav_bake_reader read_shipped,
                     unsigned char **out_bytes, size_t *out_len)
{
    char module[SH_NAVR_MODULE_CAP], cls[32];
    char why[BAKE_REASON_CAP];
    bake_module *m;
    int hit = 0, i, known = 0;

    if (!name || !out_bytes || !out_len) return 0;
    *out_bytes = NULL;
    *out_len = 0;
    if (InterlockedCompareExchange(&g_faulted, 0, 0) != 0) return 0;
    if (!bake_parse_name(name, module, sizeof module, cls, sizeof cls)) return 0;
    for (i = 0; i < NAV_CLASS_COUNT; i++) if (strcmp(cls, NAV_CLASSES[i]) == 0) known = 1;
    if (!known) return 0;

    AcquireSRWLockExclusive(&g_bake_lock);
    if (!g_have_map) { ReleaseSRWLockExclusive(&g_bake_lock); return 0; }
    /* Once per map load. The three nav classes are built back to back from one
     * editor state, so re-scanning every entity for each would cost three full
     * passes for identical answers. */
    if (!g_live_refreshed) {
        g_live_refreshed = 1;
        bake_refresh_live_locked();
    }
    m = bake_find(module);
    if (m && m->ok) {
        why[0] = 0;
        __try {
            hit = bake_one(name, m, read_shipped, out_bytes, out_len, why, sizeof why);
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
    return hit;
}

void sh_nav_bake_report(void (*out)(const char *fmt, ...))
{
    int i;
    if (!out) return;
    if (InterlockedCompareExchange(&g_faulted, 0, 0) != 0)
        out("navigation: baking is OFF for this session after a fault.\n");
    AcquireSRWLockShared(&g_bake_lock);
    if (g_module_count == 0) {
        out("navigation: no volume in this map is marked for AI navigation.\n");
    } else {
        for (i = 0; i < g_module_count; i++) {
            const bake_module *m = &g_modules[i];
            out("  marked %s -- %d volume(s) in copy %d -- %s\n",
                m->module, m->regions, m->instance,
                m->ok ? (m->reason[0] ? m->reason : "ready") : m->reason);
        }
    }
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
    memset(&g_map, 0, sizeof g_map);
    memset(g_modules, 0, sizeof g_modules);
    g_module_count = 0;
    g_have_map = 0;
    ReleaseSRWLockExclusive(&g_bake_lock);
    InterlockedExchange(&g_bakes, 0);
    InterlockedExchange(&g_faulted, 0);
}

int sh_nav_bake_test_bake_count(void)
{
    return (int)InterlockedCompareExchange(&g_bakes, 0, 0);
}
#endif
