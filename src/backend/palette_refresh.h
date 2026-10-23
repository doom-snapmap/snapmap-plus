/* palette_refresh.h -- rebuild of the live SnapMap entity palette after registration. */
#ifndef BACKEND_PALETTE_REFRESH_H
#define BACKEND_PALETTE_REFRESH_H

#include <stddef.h>
#include <stdint.h>
#include "signatures.h"

/* Resolve and arm the native palette builder. The builder is not called here:
 * the complete-registration command invokes this service on DOOM's main thread
 * after its final native scan succeeds. Install happens once per process. */
int sh_palette_refresh_install(const sig_result *results, size_t count,
                               const uint8_t *module_base);

/* Rebuild the entity palette after complete registration/materialization at a
 * boundary where existing palette consumers are released. Live added-only
 * source admission uses retained entries below and preserves native storage.
 * Returns 1 on completion. Refusal or exception is terminal for this process.
 */
int sh_palette_refresh_after_decl_registration(void);
/* Whether the live palette still holds exactly the rows the last completed
 * rebuild produced: the same row array and count, and in each row the same
 * editor declaration and entity definition. Authoring publication, a native
 * rebuild or a cleared palette all fail it. A registration pass that changed
 * no editor or entity template row may keep such a palette (R-398). */
int sh_palette_refresh_current(void);
/* Exact editor rows: a found native result may be steered among rows of an
 * equal key while an editor construction is in progress on the calling
 * thread (sh_package_editor_row_steer). A miss is never steered. */
typedef const void *(*sh_palette_steer_fn)(void *palette, const char *name, const void *found, int *outcome);
void sh_palette_refresh_steer(sh_palette_steer_fn steer);
/* Called after every completed rebuild, authoring publication and batch end:
 * row pointers recorded before it are never compared again. */
void sh_palette_refresh_rows_retired(void (*retired)(void));
/* Names of product-generated lookup rows (R-255 variant editor rows) and of
 * equal exposures another row stands for (package_runtime.c). Native
 * keep-filtered pickers leave them out; lookups still find them. Copies the
 * names; an empty call clears the set. */
void sh_palette_refresh_internal_rows(const char *const *names, size_t count);

typedef struct sh_palette_entries sh_palette_entries;
typedef struct sh_palette_entry_source {
    const char *entity_def_name; /* Exact native byte name, copied on prepare. */
    void *snap_editor_entity;    /* Already materialized native declaration. */
} sh_palette_entry_source;
typedef int (*sh_palette_entries_admitted_fn)(void *context);

/* Added-only native lookup support; never changes the native palette array or
 * GUI/category storage. The callback must prove the engine thread, heap-zero
 * allocation scope, and a quiescent native admission/retirement boundary.
 * Each source declaration must remain retained until end succeeds.
 *
 * Prepare returns an invisible owned batch; *out must initially be NULL.
 * Identical exact names/declarations share a template with counted owners.
 * Publish is all-or-nothing and native lookup always wins over the extension.
 * Retain adds a handle reference without changing lookup visibility.
 * End releases one handle, unpublishing/destroying its final native ownership
 * before clearing *batch. Call it before retiring source declarations.
 *
 * prepare/publish/end return 1 on success, 0 on ordinary refusal, -1 on unsafe
 * native failure. On -1 a non-NULL batch is quarantined and MUST keep its source
 * owner alive; no partial native object is blindly destroyed. On 0, end retains
 * the caller's handle for a later admitted retry. No arbitrary-thread teardown.
 */
int sh_palette_entries_ready(void);
/* The live editor palette (editor singleton), validated now; NULL otherwise. */
void *sh_palette_refresh_editor_palette(void);
/* Read-only selection receipt for the actual native lower_bound and published
 * added-only rows. Never calls a finder, constructor or steering callback.
 * 1 found, 0 established miss, -1 unavailable; both complete outcomes return
 * a receipt. It owns metadata only: current() must succeed before use, under
 * the caller's existing admitted save boundary. Retire/rebuild invalidates it
 * even if native storage is reused. Unknown/active steering is unavailable. */
typedef struct sh_palette_lookup_snapshot sh_palette_lookup_snapshot;
int sh_palette_lookup_capture(void *palette, const char *name,
    sh_palette_lookup_snapshot **out, void **declaration, char *error, size_t capacity);
int sh_palette_lookup_current(const sh_palette_lookup_snapshot *snapshot);
void sh_palette_lookup_release(sh_palette_lookup_snapshot *snapshot);
int sh_palette_entries_prepare(const sh_palette_entry_source *sources, size_t count,
    sh_palette_entries_admitted_fn admitted, void *context, sh_palette_entries **out,
    char *error, size_t capacity);
/* Upgrade an invisible batch to authoring publication. Copies the whole native
 * palette and its current filtered view into independently owned storage, then
 * adds only exact declarations absent from the full native array. Publish
 * exchanges headers at the admitted renderer boundary and refreshes an active
 * native add picker. Other pickers retain their current view until their normal
 * native reset. Old arrays remain owned until rollback or positive retirement.
 * The source owner must cover the existing palette as well as all added rows.
 */
int sh_palette_entries_prepare_authoring(sh_palette_entries *batch,
    sh_palette_entries_admitted_fn admitted, void *context, char *error, size_t capacity);
int sh_palette_entries_publish(sh_palette_entries *batch,
    sh_palette_entries_admitted_fn admitted, void *context, char *error, size_t capacity);
/* Allocation-free after successful external commit. Before accept, end restores
 * the original native palette. After accept, end requires positive GUI/renderer
 * retirement after a successful native destroy-and-rebuild boundary. That
 * boundary is required even when the allocator reuses the same array address.
 */
void sh_palette_entries_accept(sh_palette_entries *batch);
int sh_palette_entries_retain(sh_palette_entries *batch);
int sh_palette_entries_end(sh_palette_entries **batch,
    sh_palette_entries_admitted_fn admitted, void *context, char *error, size_t capacity);

#ifdef SH_PALETTE_REFRESH_TESTING
enum sh_palette_refresh_test_state {
    SH_PALETTE_REFRESH_TEST_IDLE = 0,
    SH_PALETTE_REFRESH_TEST_PENDING,
    SH_PALETTE_REFRESH_TEST_APPLIED,
    SH_PALETTE_REFRESH_TEST_REFUSED
};

void sh_palette_refresh_test_reset(void);
void sh_palette_refresh_test_bind(const uint8_t *module_base, void *builder);
int sh_palette_refresh_test_state(void);
int sh_palette_refresh_test_call_count(void);
void sh_palette_refresh_test_decoder(void *decoder);
void sh_palette_refresh_test_decode(void *entity, void *data);
void sh_palette_entries_test_bind(void *find, void *construct, void *valid, void *destroy);
void *sh_palette_entries_test_find(void *palette, const char *name);
size_t sh_palette_entries_test_count(void);
void sh_palette_entries_test_discard_quarantine(sh_palette_entries **batch);
typedef struct sh_palette_authoring_test_api {
    void *resize, *grow_groups, *grow_labels, *copy_body, *copy_string, *destroy;
    void *prepare_add, *prepare_filter, *refresh_widget;
    void *mode_vtable, *add_vtable, *filter_vtable;
} sh_palette_authoring_test_api;
void sh_palette_pickers_test_bind(void *partition);
void *sh_palette_pickers_test_partition(void *first, void *last, void *closure);
void sh_palette_pickers_test_bind_keep(int n, void *original);
void *sh_palette_pickers_test_keep(int n, void *first, void *last, void *predicate);
long sh_palette_pickers_test_unlisted(void);
void sh_palette_authoring_test_bind(const sh_palette_authoring_test_api *api);
#endif

#endif /* BACKEND_PALETTE_REFRESH_H */
