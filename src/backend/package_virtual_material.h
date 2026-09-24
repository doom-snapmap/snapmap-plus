/* Scoped source atlas rows connected to linker identities and native views. */
#ifndef SH_PACKAGE_VIRTUAL_MATERIAL_H
#define SH_PACKAGE_VIRTUAL_MATERIAL_H
#include "package_link_inputs.h"
#include "virtual_material_runtime.h"
#include "package_virtual_page_source.h"

typedef struct sh_package_virtual_materials sh_package_virtual_materials;
typedef struct sh_package_virtual_atlas {
    const char *name; /* Resolved native page-provider identity. */
    const char *source; /* Native page-source filename, before .vmtr conversion. */
    /* Resolved page-data binding. Private providers identify complete immutable
     * data; the single engine-owned original may use its stable external name.
     * An authored copy can share the original only after exact byte comparison. */
    unsigned char content[32];
    int original; /* Page binding certified equal to the engine original. */
    /* Owned reference for a private page family. The caller releases it after
     * bind retains its immutable inputs. NULL denotes the external original. */
    sh_virtual_page_source *pages;
    int page_kind, page_mode;
} sh_package_virtual_atlas;

/* Native filename rule: remove the source extension, append _dlc1.._dlc5
 * for those slots, then .vmtr. Slot zero has no suffix. Owned canonical path. */
char *sh_package_virtual_material_path(const char *source, unsigned slot);
/* 1 is a verified native table/page companion, 0 an unrelated path, -1 an
 * incomplete or unsupported source. Recognition never admits a native unit;
 * the material adapter must retain/project every recognized authored member. */
int sh_package_virtual_material_auxiliary(const sh_package_compilation *scope,
    const sh_compiled_resource *file, char *error, size_t capacity);

/* Scopes are the same immutable compilations passed to source_link. Tables
 * resolve locally, then through captured originals; peer scopes never fill a
 * hole. The original reader must include loose engine atlas tables as well as
 * archive files and distinguish absence from failure. All snapshots are private
 * and independent of active renderer objects. No native calls or file writes. */
sh_package_virtual_materials *sh_package_virtual_material_open(
    const sh_package_compilation *const *scopes, size_t count,
    sh_package_baseline_reader original, void *context);
void sh_package_virtual_material_close(sh_package_virtual_materials *materials);

/* Called by the native source-semantics adapter, after it resolves the material's
 * page-provider selection, including native aliasing for distinct page data.
 * One resolved provider name cannot denote different page data in one view.
 * Hash the selected native row AND page-data binding,
 * not unrelated rows or package IDs. Six native table slots resolve in engine
 * order; a later slot replaces an earlier same-name row. A missing row is valid
 * for a nonvirtual material. Generic materials bypass native VMTR lookup.
 * Return 1 for a retained row, 0 for no row, -1 for incomplete/invalid binding.
 * semantics/original describe the implicit binding only; source_link checks
 * the material bytes and the linker checks explicit dependencies separately.
 * One scoped material must have one stable atlas selection in this snapshot. */
int sh_package_virtual_material_bind(sh_package_virtual_materials *materials,
    size_t scope, const char *material, const sh_package_virtual_atlas *atlas,
    unsigned char semantics[32], int *original, char *error, size_t capacity);
int sh_package_virtual_material_witnesses(sh_package_virtual_materials *materials,
    size_t scope, const char *material,
    int (*visit)(void *, const char *, const sh_compiled_resource *), void *context,
    char *error, size_t capacity);
/* Reuse the already frozen native selection; never recapture the same scoped
 * material during companion collection and the later semantics walk. */
int sh_package_virtual_material_cached(const sh_package_virtual_materials *materials,
    size_t scope, const char *material, unsigned char semantics[32], int *original);
/* Enumerate affected material identities from authored VMTR/page families.
 * Scoped and original rows are both visited: deleting a row still changes the
 * material that previously selected it. Visitors deduplicate native names. */
int sh_package_virtual_material_seeds(sh_package_virtual_materials *materials,
    size_t scope, int (*visit)(void *, const char *), void *context,
    char *error, size_t capacity);

/* Build the actual immutable runtime view for the linked material aliases.
 * Every material unit must have been inspected above, including no-row cases.
 * Exact duplicates share a binding; differing page data cannot share an alias.
 * The runtime view owns its copies and outlives this source snapshot. Original
 * unchanged names use the engine's own lookup. This does not publish the view. */
sh_virtual_material_view *sh_package_virtual_material_prepare(
    sh_package_virtual_materials *materials, const sh_package_link_inputs *inputs,
    sh_package_link_output *linked, char *error, size_t capacity);
#endif
