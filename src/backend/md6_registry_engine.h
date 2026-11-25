/* Per-Play md6Def carriers. The game thread publishes registries and loads models. */
#ifndef SH_MD6_REGISTRY_ENGINE_H
#define SH_MD6_REGISTRY_ENGINE_H
#include <windows.h>
#include "md6_registry.h"

typedef struct sh_md6_runtime {
    sh_md6_registry *volatile current;
    sh_md6_registry *retired;
    volatile LONG any;
} sh_md6_runtime;

int sh_md6_av_only(DWORD code);
int sh_md6_parent(void *context, const void *model, const void **parent);
const char *sh_md6_name_at(const void *object, size_t offset);
void *sh_md6_model(sh_md6_row *row, sh_md6_registry_kind kind);
sh_md6_registry *sh_md6_registry_build(sh_md6_registry_kind kind, int *oom);
void sh_md6_registry_publish(sh_md6_runtime *runtime, sh_md6_registry *rows);
void sh_md6_registry_print(const sh_md6_registry *rows, sh_md6_registry_kind kind, sh_ce_line_fn line, void *context);
void sh_md6_log(const char *fmt, ...);
#endif
