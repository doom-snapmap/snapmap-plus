#ifndef SH_RESOURCE_PRESENTATION_H
#define SH_RESOURCE_PRESENTATION_H
#include <stddef.h>
#include <stdint.h>
#include "resource_graph.h"
/* Value tokens only; graph and instance records belong to their FLS owners. */
typedef struct sh_resource_presentation_scope {
    sh_resource_graph_frame graph;
    uint64_t context,serial;
    int opened;
} sh_resource_presentation_scope;
typedef int (*sh_resource_presentation_begin)(sh_resource_presentation_scope *);
typedef void (*sh_resource_presentation_end)(sh_resource_presentation_scope *);
int sh_resource_presentation_install(const uint8_t *base,
    sh_resource_presentation_begin begin,sh_resource_presentation_end end);
int sh_resource_presentation_ready(void);
/* Native execution must be quiescent before removal. */
int sh_resource_presentation_uninstall(void);
#endif
