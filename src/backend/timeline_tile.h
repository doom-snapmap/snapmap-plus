/* timeline_tile.h -- the Timeline palette tile on the native entityDef
 * snapmaps/unknown.
 *
 * The tile is a product-served sedef (custom/timeline, overrides_baked.h)
 * that DOOM never shipped, so it is registered with the decl registry just
 * before the editor builds its palette. Its row's prototype record (palette
 * entry +0x150) is then given class idTarget_Timeline, which every preview
 * and toybox placement from the row copies, so a placed timeline is
 * className idTarget_Timeline, inherit snapmaps/unknown from creation, under
 * the native "Add Entity" undo step. No entityDef is overridden.
 *
 * Map load and paste bind an entity to the first palette row whose inherit
 * matches, and the two rows on snapmaps/unknown sort in an unstable order.
 * After the entity list changes, entities on snapmaps/unknown are rebound by
 * class: idTarget_Timeline to the tile, everything else to the Unknown row.
 * Both rows carry the same flag bytes, reference-slot counts and wiring
 * lists, so a pointer rebind leaves nothing derived from the old row stale. */
#ifndef SH_TIMELINE_TILE_H
#define SH_TIMELINE_TILE_H

#include <stddef.h>
#include <stdint.h>

#define SH_TL_TILE_SEDEF      "custom/timeline"
#define SH_TL_TILE_PATH       "snapeditorentitydef/custom/timeline.decl"
#define SH_TL_UNKNOWN_SEDEF   "unknown/unknown"
#define SH_TL_INHERIT         "snapmaps/unknown"
#define SH_TL_CLASS           "idTarget_Timeline"
#define SH_TL_FIRST_ENTITY    56     /* ids below are the map's built-in entities */

/* idSnapEntity JSON for the tile's prototype record: inherit snapmaps/unknown,
 * class idTarget_Timeline, an empty edit block. */
extern const char sh_tl_record_json[];

/* The rebind decision for one entity. Returns the sedef the entity should be
 * bound to, or NULL to leave it alone: entities whose inherit is not
 * snapmaps/unknown, entities bound to some other row, and any case where a
 * row is missing. */
const void *sh_tl_wanted_sedef(const char *inherit, const char *cls, const void *current,
                               const void *tile, const void *unknown);

/* EntityMode substates that hold an entity in the editor's hands (held
 * placement 4, wire drag 7). Entities are not rebound during them. */
int sh_tl_substate_blocks_rebind(int substate);

#ifndef SH_TIMELINE_TILE_PURE
#include "signatures.h"
#include "snapmap_plus_iface.h"

/* Bind the decl registry and the palette build. Returns 1 when the tile can
 * be registered (the per-frame record and rebind work needs only the apply
 * engine). */
int sh_timeline_tile_install(const sig_result *results, size_t count, const uint8_t *module_base);

/* Called by the EditorFrame detour after the engine's frame, on the game thread. */
void sh_timeline_tile_tick(void *editor);

/* Rebuild the live editor palette inside process heap 0 (see timeline_tile.c).
 * Main thread only. Row counts before and after are written when non-NULL. */
enum {
    SH_TL_REBUILD_OK = 1,
    SH_TL_REBUILD_UNAVAILABLE = 0,    /* no editor seen, or the build hook is absent */
    SH_TL_REBUILD_BUSY = -1,          /* an entity or a wire is held */
    SH_TL_REBUILD_HEAP = -2,          /* process heap 0 could not be entered */
    SH_TL_REBUILD_DISABLED = -3,      /* an earlier rebuild left the heap scope unbalanced */
    SH_TL_REBUILD_FAULTED = -4,       /* the builder faulted */
    SH_TL_REBUILD_HEAP_LEAVE = -5     /* the scope did not balance; rebuilds now refuse */
};
int sh_timeline_tile_rebuild_palette(int *rows_before, int *rows_after);

/* The +0x368 slot body (see snapmap_plus_iface.h). */
int sh_timeline_tile_slot(struct sh_iface *self, sh_timeline_tile_call *call);
#endif

#endif
