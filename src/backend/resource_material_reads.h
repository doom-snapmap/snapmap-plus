/* Reached registered program inputs of the paired native Material parser. */
#ifndef SH_RESOURCE_MATERIAL_READS_H
#define SH_RESOURCE_MATERIAL_READS_H
#include <stddef.h>
#include <stdint.h>

/* Synchronous actual-resource observer. NULL reports an unknown reached input.
 * The callback must retain the actual opening origin for the caller's active
 * owner and must not call native code or reenter this module. Callback failure
 * never suppresses or replaces native execution. */
int sh_resource_material_reads_install(const uint8_t *base, int (*observe)(void *));
int sh_resource_material_reads_ready(void);

#ifdef SH_RESOURCE_MATERIAL_READS_TESTING
typedef int (*sh_resource_material_reader)(void *, uintptr_t, void *, size_t);
/* SORT_COVERAGE second points to a synchronous native float result. */
enum { SH_MATERIAL_COPY, SH_MATERIAL_EARLY_FLAGS, SH_MATERIAL_HAS_PARM, SH_MATERIAL_SORT_COVERAGE, SH_MATERIAL_VIDEO_MAP };
int sh_resource_material_reads_test_probe(int kind, void *first, void *second,
    sh_resource_material_reader reader, void *context, int (*observe)(void *));
void sh_resource_material_reads_test_reset(void);
#endif
#endif

