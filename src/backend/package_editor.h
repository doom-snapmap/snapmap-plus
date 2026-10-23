/* Package provenance at verified native editor object lifetime boundaries. */
#ifndef SH_PACKAGE_EDITOR_H
#define SH_PACKAGE_EDITOR_H
#include "package_occurrence.h"
#include "signatures.h"

enum { SH_PACKAGE_EDITOR_MAP = 1, SH_PACKAGE_EDITOR_PREFAB = 2, SH_PACKAGE_EDITOR_ENTITY = 3 };
/* Called only from native editor work on its own thread. Inspect exact native
 * serialization and return a complete document snapshot. No partial ownership
 * is attached on failure. previous is borrowed for this call. */
typedef int (*sh_package_editor_inspect_fn)(void *context, unsigned kind, void *flat,
    const sh_package_occurrence_snapshot *previous, sh_package_occurrence_snapshot **next,
    char *error, size_t capacity);
void sh_package_editor_bind(sh_package_editor_inspect_fn inspect, void *context);
/* Install the production serializer/typed-reader adapter. */
void sh_package_editor_bind_native(void);
/* Native targets must be quiescent during install/removal. Every target and
 * complete stolen prologue is verified before any detour is committed. */
int sh_package_editor_install(const sig_result *results, size_t count, const uint8_t *base);
int sh_package_editor_uninstall(void);
int sh_package_editor_ready(void);
/* A failed ownership transfer is never silently treated as vanilla on save. */
int sh_package_editor_healthy(char *error, size_t capacity);
/* MAP: flat idSnapMap; PREFAB: idSnapEntityPrefab; ENTITY: idSnapEntity.
 * Snapshot order comes from the actual contiguous native array, never uniqueId.
 * Returned document is owned; attach borrows it. Caller serializes with native
 * mutation. Map world ownership is included only for MAP operations. */
sh_package_occurrence_snapshot *sh_package_editor_snapshot(unsigned kind, const void *flat,
    char *error, size_t capacity);
int sh_package_editor_attach(unsigned kind, const void *flat,
    const sh_package_occurrence_snapshot *document, char *error, size_t capacity);
int sh_package_editor_inspect(unsigned kind, void *flat, char *error, size_t capacity);
/* Retire current world policy after confirmed map departure. Clipboard/undo
 * bodies retain their own snapshots until their native destructors execute. */
void sh_package_editor_clear_world(void);
/* Caller retains this across a native load transaction and restores it only
 * when that load rolls back. Returned handle is owned; restore borrows. */
sh_package_occurrence_snapshot *sh_package_editor_world(void);
void sh_package_editor_restore_world(sh_package_occurrence_snapshot *world);
/* Exact editor rows (R-155, R-155b). Rows sharing an entityDef are equal to
 * the engine's by-name lookup, whose tie order follows an unstable re-sort, so
 * array order is never authored identity. A body's row (its first qword) is
 * recorded per object only when the product published it, and steers the
 * lookup by declaration identity during construction; an unrecorded tie takes
 * its single native-original row, or without one its single row in a native
 * SnapMap slot, built but never captured as authored; an added row never wins
 * by order. The choice never selects suppliers; snapshot
 * exports each flat's named record as its owner's optional exact row, and
 * attach restores recorded rows as flat records before any construction.
 * Engine access is injected: palette view, row key (NULL when unreadable, as a
 * null holder is during Play) and row class (1 published, writing its stable
 * name; 2 published in a native SnapMap slot, likewise; 0 native-original;
 * -1 unknown). resolve maps a published name to the
 * live row after the palette generation is retired. */
typedef struct sh_package_editor_rows_api {
    int (*view)(void *context, void *palette, const unsigned char **entries, size_t *count, size_t *stride);
    const char *(*key)(void *context, const void *entry);
    int (*classify)(void *context, const void *decl, char *name, size_t capacity);
    const void *(*resolve)(void *context, const char *name);
    /* 1 when decl's publication lineage is a map-authoritative supplying
     * context of owner (the object's occurrence snapshot). */
    int (*supplied)(void *context, const void *owner, const void *decl);
    void *context;
    /* 1 when decl's publication carries name as an identity of its own (the
     * portable one, or an earlier map's); NULL compares classify's name. */
    int (*named)(void *context, const void *decl, const char *name);
    /* Writes decl's exact publication identity (its registered bytes, so a
     * dependency variant differs); 1 written. A captured choice keeps it in
     * this process only: never saved, it is the authority by which a retired
     * choice re-resolves (with the owner's supplier when owned), so a variant
     * sharing the portable identity is never substituted. NULL: none kept. */
    int (*exact)(void *context, const void *decl, char *name, size_t capacity);
    /* 1 when the served exposure selection leaves decl's row out of the
     * pickers and candidate is the equal row it keeps for it
     * (package_runtime.h sh_package_runtime_exposure_kept); 0 when not, -1
     * unknown. NULL: no selection. */
    int (*stands_for)(void *context, const void *decl, const void *candidate);
} sh_package_editor_rows_api;
/* Outcomes up to NATIVE_DEFAULT are authoritative; NATIVE_DEFAULT applies only
 * to unowned objects without a record. The rest retain the choice unresolved.
 * EXPOSURE: such an object whose native default left the pickers for an equal
 * exposure is built from the tie that exposure keeps (stands_for) instead, and
 * nothing is captured. An owned object without a record and with no supplier
 * for its key (OWNED_NONE) is built from that tie too, keeping its unresolved
 * outcome. A record or a supplier is never replaced by it. */
enum { SH_ROW_ENGINE, SH_ROW_RECORDED, SH_ROW_DERIVED, SH_ROW_NATIVE_DEFAULT, SH_ROW_UNAVAILABLE,
    SH_ROW_OWNED_NONE, SH_ROW_OWNED_AMBIGUOUS, SH_ROW_AMBIGUOUS, SH_ROW_UNREADABLE, SH_ROW_EXPOSURE };
void sh_package_editor_bind_rows(const sh_package_editor_rows_api *api);
/* Native palette or decl generation retired (callers quiesce first): stored
 * row pointers are never compared again; names re-resolve on next use. */
void sh_package_editor_rows_retire(void);
/* The by-name lookup's result steered for the construction in progress on
 * this thread; outcome may be NULL. Pure over the bound view; no cap on ties. */
/* Whether this thread is constructing an occurrence with exact-row steering. */
int sh_package_editor_row_steering_active(void);
const void *sh_package_editor_row_steer(void *palette, const char *name, const void *engine, int *outcome);
#endif
