/* Native lookup publication for compiler-resolved virtual material bindings. */
#ifndef SH_VIRTUAL_MATERIAL_RUNTIME_H
#define SH_VIRTUAL_MATERIAL_RUNTIME_H
#include "virtual_material.h"
#include "signatures.h"
#include "package_virtual_page_source.h"

typedef struct sh_virtual_material_view sh_virtual_material_view;
typedef struct sh_virtual_material_runtime_binding {
    const char *name; /* Compiled native material identity. */
    const char *atlas; /* Resolved native page-provider name, not a source path. */
    const sh_virtual_material_record *record;
    int32_t dlc; /* Native table-origin value, supplied by the atlas adapter. */
    sh_virtual_page_source *pages; /* Retained complete private source, or NULL. */
    int page_kind, page_mode;
    const char *const *page_paths; /* 21 exact final compiler paths; copied. */
} sh_virtual_material_runtime_binding;

/* Copy and validate a complete candidate without native calls or publication.
 * Equal bindings collapse. Same-name bindings with unequal atlas/record data
 * refuse the candidate. No author fields, engine objects or global state change.
 * The linker must include resolved page data in the material's equivalence key;
 * equal rectangle coordinates alone do not establish texture equivalence. */
sh_virtual_material_view *sh_virtual_material_runtime_prepare(
    const sh_virtual_material_runtime_binding *bindings, size_t count,
    char *error, size_t capacity);
/* Copy immutable bindings from two provider views. Equal aliases collapse;
 * disagreement fails before publication. Native rows are never copied. */
sh_virtual_material_view *sh_virtual_material_runtime_join(
    const sh_virtual_material_view *a, const sh_virtual_material_view *b,
    char *error, size_t capacity);
/* Copy only selected immutable bindings. The predicate sees canonical material
 * names and performs no native calls. Negative result aborts the whole copy. */
sh_virtual_material_view *sh_virtual_material_runtime_filter(
    const sh_virtual_material_view *source, int (*keep)(void *, const char *), void *context,
    char *error, size_t capacity);
/* Immutable binding comparison for the provider's native refresh diff. name is
 * the canonical material identity. Native cache pointers/status do not enter it. */
int sh_virtual_material_runtime_binding_equal(const sh_virtual_material_view *a,
    const sh_virtual_material_view *b, const char *name);
/* After ordinary compiler caching, before native preparation. Retain and
 * positively compare every exact projected page stream. No native calls. */
int sh_virtual_material_runtime_seal(sh_virtual_material_view *view,
    const sh_package_compilation *compiled, char *error, size_t capacity);

/* Main-thread provider/render barrier only. Publish atomically with its resource
 * provider; retain previous for rollback. NULL selects native-only lookup.
 * A live alias cannot acquire a different meaning. Failure leaves both outputs
 * and the current selection intact. Nonempty views require an installed hook.
 * The caller retains every published handle until release succeeds. */
int sh_virtual_material_runtime_publish(sh_virtual_material_view *candidate,
    sh_virtual_material_view **previous, char *error, size_t capacity);
/* Restore the retained previous handle from publish, under the same serialized
 * transaction/barrier. No allocation, validation or native calls can obstruct
 * rollback. The transaction must not retire either handle before this call. */
void sh_virtual_material_runtime_restore(sh_virtual_material_view *previous);
/* Main-thread provider/render barrier only. Free detached rows. release refuses
 * an active view or any live native association and retains the caller handle.
 * An inactive view can still own rows used by equivalent bindings in the active
 * provider. Native material destruction clears each association before release. */
void sh_virtual_material_runtime_collect(void);
int sh_virtual_material_runtime_release(sh_virtual_material_view **view);
/* Drop a provider's inactive handle without retaining its source tree. Published
 * rows remain owned by the registry until collect observes native destruction
 * inside a renderer barrier. Unpublished views can be freed immediately. This
 * does not inspect/reclaim native associations and needs no renderer barrier.
 * An active view cannot be retired; failure preserves the handle. */
int sh_virtual_material_runtime_retire(sh_virtual_material_view **view);
/* Activation must check this after all native loads, before committing. A lookup
 * failure belonging to the selected provider remains visible until it is retired;
 * it is never converted to an unrelated original texture or hidden by a fallback. */
int sh_virtual_material_runtime_status(char *error, size_t capacity);
/* A retained failed view requires native reconstruction on retry even when
 * immutable file/binding bytes are unchanged. NULL is a healthy empty view. */
int sh_virtual_material_runtime_healthy(const sh_virtual_material_view *view);
int sh_virtual_material_runtime_install(const sig_result *results, size_t count);
int sh_virtual_material_runtime_ready(void);
#endif
