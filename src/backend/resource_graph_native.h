#ifndef SH_RESOURCE_GRAPH_NATIVE_H
#define SH_RESOURCE_GRAPH_NATIVE_H
#include "signatures.h"
int sh_resource_graph_native_signature(const char *name);
int sh_resource_graph_native_install(const sig_result *results, size_t count);
int sh_resource_graph_native_ready(void);
#endif
