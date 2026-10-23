/* Exact material ownership at particle-light and committed-light boundaries. */
#ifndef SH_RESOURCE_LIGHT_ORIGINS_H
#define SH_RESOURCE_LIGHT_ORIGINS_H
#include <stdint.h>
#include <stddef.h>
int sh_resource_light_origins_install(const uint8_t *base);
int sh_resource_light_origins_ready(void);
/* -1 other kind, 0 incomplete, 1 positively replaced/retired. */
int sh_resource_light_origins_reconcile(void *object,uint64_t token,char *error,size_t capacity);
/* Caller has quiesced native hook execution. Failed removal retains ownership. */
int sh_resource_light_origins_uninstall(void);
/* Under resident's native render/heap fence. changed is the batch's keyed
 * predicate over actual resources rebuilt in place. */
int sh_resource_light_origins_refresh(int (*changed)(void *,void *),void *context,char *error,size_t capacity);
#endif
