/* Private virtual-material rows returned by the compiler's native lookup. */
#ifndef SH_VIRTUAL_MATERIAL_NATIVE_H
#define SH_VIRTUAL_MATERIAL_NATIVE_H
#include "virtual_material.h"

typedef struct sh_virtual_material_native sh_virtual_material_native;

/* Each bound material needs its own mutable association even when several
 * share one immutable atlas rectangle. owner is the resolved native atlas
 * context, not its static page-provider entry; extent belongs to that provider.
 * This object is never inserted into an engine-owned/freeable table. */
sh_virtual_material_native *sh_virtual_material_native_create(
    const sh_virtual_material_record *source, const char *name,
    uintptr_t owner, int32_t extent, int32_t dlc, char *error, size_t capacity);

/* Returns the ABI row to the lookup hook. Native Parse stores it at material
 * +0x170 and applies its mapping. The native destructor clears its material
 * backpointer before dropping that field. A different live material cannot
 * take an already-bound row. This never edits an engine object itself. */
void *sh_virtual_material_native_attach(sh_virtual_material_native *record, uintptr_t material);

/* Main-thread native refresh barrier only: no render/loader consumer may run
 * while calling these operations. A provider change may update extent/owner
 * for the same resolved atlas identity; the caller must separately prove that
 * identity and then apply the material's VMTR scale/bias setter (the global
 * virtual-texture page rebind does not update this mapping). Failure leaves
 * the old mapping intact. No reassignment to unrelated page data is allowed. */
int sh_virtual_material_native_refresh(sh_virtual_material_native *record,
    uintptr_t owner, int32_t extent);
/* Refuse to free a row still associated with a material. After native material
 * destruction/reconstruction has cleared it, the same render barrier permits
 * reclamation. Failure retains the handle for the provider's cleanup retry. */
int sh_virtual_material_native_release(sh_virtual_material_native **record);
/* Read-only association inspection, under the same caller synchronization. */
void *sh_virtual_material_native_row(const sh_virtual_material_native *record);
uintptr_t sh_virtual_material_native_material(const sh_virtual_material_native *record);
int sh_virtual_material_native_atlas(const sh_virtual_material_native *record,
    uintptr_t owner, int32_t extent);
#endif
