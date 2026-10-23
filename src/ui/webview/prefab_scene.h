/* Renderable prefab view; complete authored deliveries stay in the source file. */
#ifndef SH_PREFAB_SCENE_H
#define SH_PREFAB_SCENE_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Return 1 with malloc-owned scene JSON and the direct entity count. The native
 * JSON reader preserves engine byte strings; no author text is rewritten. */
int sh_prefab_scene(const char *json, size_t length, char **scene, size_t *scene_length, int *count);
#ifdef __cplusplus
}
#endif
#endif
