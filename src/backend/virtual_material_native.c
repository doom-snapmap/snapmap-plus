#include "virtual_material_native.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Both retail x64 renderers use this row. Their parser allocates 0x50 bytes;
 * lookup sets +0x30, scale derives +0x38 from pixel coordinates and records
 * its atlas owner at +0x48. idMaterial destruction clears +0x30, not the row.
 * Ownership stays private: native table destruction must never free it. */
typedef struct vm_native_row {
    const char *name;
    int32_t checksum, timestamp;
    uint64_t reserved;
    int32_t x, y, width, height, flags, dlc;
    uintptr_t material;
    float mapping[4];
    uintptr_t owner;
} vm_native_row;
typedef char vmn_size_check[sizeof(vm_native_row) == 0x50 ? 1 : -1];
typedef char vmn_pixels_check[offsetof(vm_native_row, x) == 0x18 ? 1 : -1];
typedef char vmn_material_check[offsetof(vm_native_row, material) == 0x30 ? 1 : -1];
typedef char vmn_mapping_check[offsetof(vm_native_row, mapping) == 0x38 ? 1 : -1];
typedef char vmn_owner_check[offsetof(vm_native_row, owner) == 0x48 ? 1 : -1];

struct sh_virtual_material_native { vm_native_row row; int32_t extent; };

static void vmn_error(char *error, size_t capacity, const char *reason)
{ if (error && capacity) snprintf(error, capacity, "%s", reason); }

sh_virtual_material_native *sh_virtual_material_native_create(
    const sh_virtual_material_record *source, const char *name,
    uintptr_t owner, int32_t extent, int32_t dlc, char *error, size_t capacity)
{
    sh_virtual_material_native *out = NULL;
    float mapping[4]; char *canonical = NULL;
    if (error && capacity) error[0] = 0;
    if (!owner || !sh_virtual_material_mapping(source, extent, mapping)) {
        vmn_error(error, capacity, "virtual material does not fit its resolved native atlas"); return NULL;
    }
    canonical = sh_virtual_material_name(name, name ? strlen(name) : 0);
    if (!canonical) { vmn_error(error, capacity, "invalid virtual material binding name"); return NULL; }
    out = calloc(1, sizeof(*out));
    if (!out) { vmn_error(error, capacity, "cannot retain a private virtual material row"); goto done; }
    out->row.name = canonical; canonical = NULL;
    out->row.checksum = source->checksum; out->row.timestamp = source->timestamp;
    out->row.x = source->x; out->row.y = source->y;
    out->row.width = source->width; out->row.height = source->height;
    out->row.flags = source->flags; out->row.dlc = dlc; out->row.owner = owner;
    out->extent = extent;
    memcpy(out->row.mapping, mapping, sizeof(mapping));
done:
    free(canonical); return out;
}

void *sh_virtual_material_native_attach(sh_virtual_material_native *record, uintptr_t material)
{
    if (!record || !material || (record->row.material && record->row.material != material)) return NULL;
    record->row.material = material; return &record->row;
}

int sh_virtual_material_native_refresh(sh_virtual_material_native *record,
    uintptr_t owner, int32_t extent)
{
    sh_virtual_material_record source = {0}; float mapping[4];
    if (!record || !owner) return 0;
    source.name = (char *)record->row.name;
    source.x = record->row.x; source.y = record->row.y;
    source.width = record->row.width; source.height = record->row.height;
    source.flags = record->row.flags; source.timestamp = record->row.timestamp;
    source.checksum = record->row.checksum;
    if (!sh_virtual_material_mapping(&source, extent, mapping)) return 0;
    memcpy(record->row.mapping, mapping, sizeof(mapping)); record->row.owner = owner;
    record->extent = extent; return 1;
}

int sh_virtual_material_native_release(sh_virtual_material_native **record)
{
    if (!record || !*record) return 1;
    if ((*record)->row.material) return 0;
    free((void *)(*record)->row.name); free(*record); *record = NULL; return 1;
}

void *sh_virtual_material_native_row(const sh_virtual_material_native *record)
{ return record ? (void *)&record->row : NULL; }
uintptr_t sh_virtual_material_native_material(const sh_virtual_material_native *record)
{ return record ? record->row.material : 0; }
int sh_virtual_material_native_atlas(const sh_virtual_material_native *record,
    uintptr_t owner, int32_t extent)
{ return record && record->row.owner == owner && record->extent == extent; }
