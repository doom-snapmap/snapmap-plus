/* Source records for the engine's virtual-material atlas bindings. */
#ifndef SH_VIRTUAL_MATERIAL_H
#define SH_VIRTUAL_MATERIAL_H

#include "decl_tree.h"
#include <stdint.h>

typedef struct sh_virtual_material_record {
    char *name; /* Owned canonical native identity. */
    int32_t x, y, width, height, flags, timestamp, checksum;
} sh_virtual_material_record;

typedef struct sh_virtual_material_table {
    sh_virtual_material_record *records; /* Sorted by name; duplicates collapse. */
    size_t count;
} sh_virtual_material_table;

/* Canonical native table identity, not a physical filesystem path. Returns
 * malloc-owned text using the table reader's case/slash and literal rules. */
char *sh_virtual_material_name(const char *text, size_t length);
/* Allocation-free form for native lookup. Failed conversion clears out[0]. */
int sh_virtual_material_name_copy(const char *text, size_t length, char *out, size_t capacity);

/* Read the retail text table. This is a native resource format, independent of
 * package descriptors. The source remains unchanged. Duplicate names require
 * equal record data; an ambiguous or incomplete table is never partially read.
 * Initialize out to zero. Repeated reads replace it; failure clears it. */
int sh_virtual_material_read(sh_decl_source source, sh_virtual_material_table *out,
    char *error, size_t capacity);
void sh_virtual_material_free(sh_virtual_material_table *table);
const sh_virtual_material_record *sh_virtual_material_find(
    const sh_virtual_material_table *table, const char *name);
/* Native scale/bias order is width, height, x, y divided by atlas extent.
 * Reject records outside the selected square atlas; clear output on failure. */
int sh_virtual_material_mapping(const sh_virtual_material_record *record,
    int32_t extent, float mapping[4]);

typedef struct sh_virtual_material_binding {
    const sh_virtual_material_record *record;
    const char *name; /* Compiler-assigned material identity. */
} sh_virtual_material_binding;

/* Project bindings for ONE resolved atlas. The caller must establish its page
 * provider identity separately: equal coordinates in different atlases are not
 * equal textures. Same-name/equal-record bindings collapse, differing records
 * under one name fail. Distinct material names retain independent records and
 * can share atlas coordinates. Does not create or modify native objects/files.
 * Returned text is malloc-owned, sorted, and independent of input order. */
char *sh_virtual_material_project(const sh_virtual_material_binding *bindings,
    size_t count, size_t *length, char *error, size_t capacity);

#endif
