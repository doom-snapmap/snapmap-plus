/* Actual registered-resource and cached VT inputs at native model boundaries. */
#ifndef SH_RESOURCE_MODEL_READS_H
#define SH_RESOURCE_MODEL_READS_H
#include <stdint.h>
#include "resource_instances.h"

/* Observers run synchronously in the caller's active dependency frame. They
 * must not call native code or reenter this module. NULL marks an unknown
 * actual input. observe_origin must retain a live share before returning if
 * the default frame will use it after this boundary's receipt finishes. */
int sh_resource_model_reads_install(const uint8_t *base,
    int (*observe_resource)(void *), int (*observe_origin)(sh_resource_instance_origin *));
int sh_resource_model_reads_ready(void);
/* Model and particle defaults need actual model/material observations.
 * Discreteanimation and Material additionally require actual VT provider
 * provenance and parameter evaluators. Other families return1 here. */
int sh_resource_model_reads_default_ready(const char *family);

#ifdef SH_RESOURCE_MODEL_READS_TESTING
/* Execute the production pre-read probes with an explicit safe reader. */
typedef int (*sh_resource_model_reader)(void *, uintptr_t, void *, size_t);
enum { SH_MODEL_READ_BOUNDS, SH_MODEL_READ_COMPARE, SH_MODEL_READ_VT_UPDATE, SH_MODEL_READ_VT_BIND };
int sh_resource_model_reads_test_probe(int kind, int renderer, const uint8_t *base,
    void *first, void *second, sh_resource_model_reader reader, void *context,
    int (*observe_resource)(void *));
void sh_resource_model_reads_test_reset(void);
#endif
#endif
