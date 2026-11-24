/* timeline_tile.h -- the Timeline palette tile: a product-served sedef (custom/timeline, overrides_baked.h)
 * registered just before the editor builds its palette, whose prototype record (palette entry +0x150) is given class
 * idTarget_Timeline so every placement is className idTarget_Timeline, inherit snapmaps/unknown. Map load and paste
 * bind to the first row matching an inherit and the two snapmaps/unknown rows sort unstably, so entities are rebound
 * by class after the list changes; both rows share flags, slot counts and wiring lists, so the rebind is a pointer swap. */
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

/* idSnapEntity JSON for the tile's prototype record: inherit snapmaps/unknown, class idTarget_Timeline, empty edits. */
extern const char sh_tl_record_json[];

/* The rebind decision for one entity: the sedef it should bind to, or NULL to leave it alone (inherit not
 * snapmaps/unknown, bound to some other row, or a row missing). */
const void *sh_tl_wanted_sedef(const char *inherit, const char *cls, const void *current,
                               const void *tile, const void *unknown);

/* A mod's row (sedef under mods/) sharing a stock row's entityDef sorts beside it unstably, so load and paste
 * could bind stock entities to it. Returns `stock_row` when `current_row` (the bound row's sedef name) is a mod's
 * and differs from it, else NULL; an editor tweak shadowing a stock name counts as stock. */
const void *sh_tl_stock_row_first(const char *current_row, const void *current, const void *stock_row);

/* EntityMode substates that hold an entity in the editor's hands (held
 * placement 4, wire drag 7). Entities are not rebound during them. */
int sh_tl_substate_blocks_rebind(int substate);

/* The palette sort reached through the E8 at call_site (SnapPaletteSortCall + 37) when its first 12 bytes are
 * the wrapper's prologue, else NULL. The install's own check. */
#define SH_TIMELINE_SORT_CALL_OFF 37  /* the E8 to the sort inside SnapPaletteSortCall */
const unsigned char *sh_timeline_tile_sort_target(const unsigned char *call_site);

#ifndef SH_TIMELINE_TILE_PURE
#include "signatures.h"
#include "snapmap_plus_iface.h"

/* Bind the decl registry and the palette build. Returns 1 when the tile can be registered (the per-frame
 * record and rebind work needs only the apply engine). */
int sh_timeline_tile_install(const sig_result *results, size_t count, const uint8_t *module_base);

/* Called by the EditorFrame detour after the engine's frame, on the game thread. */
void sh_timeline_tile_tick(void *editor);

/* Rebuild the live editor palette inside process heap 0 (see timeline_tile.c); main thread only. Row counts
 * before and after are written when non-NULL. */
enum {
    SH_TL_REBUILD_OK = 1,
    SH_TL_REBUILD_UNAVAILABLE = 0,    /* no editor seen, or the build hook is absent */
    SH_TL_REBUILD_BUSY = -1,          /* an entity or a wire is held */
    SH_TL_REBUILD_HEAP = -2,          /* process heap 0 could not be entered */
    SH_TL_REBUILD_DISABLED = -3,      /* a rebuild left the heap scope unbalanced; runtime rebuilds are off */
    SH_TL_REBUILD_FAULTED = -4,       /* the builder faulted */
    SH_TL_REBUILD_HEAP_LEAVE = -5     /* the scope did not balance; later rebuilds refuse */
};
int sh_timeline_tile_rebuild_palette(int *rows_before, int *rows_after);

/* The editor palette (editor+0x20660) once the builder has completed at least
 * once this process, else NULL. */
void *sh_timeline_tile_palette(void);

/* Palette sorts seen by the sort guard, how many ran outside process heap 0, and whether the guard is installed. */
void sh_timeline_tile_sorts(int *sorts, int *unscoped, int *guarded);

/* The +0x368 slot body (see snapmap_plus_iface.h). */
int sh_timeline_tile_slot(struct sh_iface *self, sh_timeline_tile_call *call);
#endif

#endif
