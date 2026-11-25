/* Play's SnapMap world-area cell size, chosen per map. BuildRenderWorld uses areas at most 512 units wide
 * and the engine refuses more than 8192 areas or nodes (the loading screen never returns). A map needing
 * more is built with the smallest coarser size (1024, 2048, ...) that fits; the rest keep 512. */
#ifndef SH_WORLD_AREA_CELL_H
#define SH_WORLD_AREA_CELL_H
#include <stdint.h>
#include "signatures.h"

/* Check the site's bytes; on success *constant is the address its disp32
 * reads. Does not read the constant. */
int sh_world_area_cell_site(const unsigned char *site,uintptr_t *constant);
/* The disp32 that makes the site read dest; 0 when dest is out of reach. */
int sh_world_area_cell_disp(uintptr_t site,uintptr_t dest,int32_t *disp);

#ifndef SH_WORLD_AREA_CELL_PURE
/* The install's byte checks (load decoded, constant 512); NULL when they hold, else the reason. */
const char *sh_world_area_cell_check(const unsigned char *site,uintptr_t *constant);
/* Boot thread, after the Grid Room native install. Logs either way. */
int sh_world_area_cell_install(const sig_result *results,size_t count,const uint8_t *base);
/* The SnapBuild detour, before the native build: count the map and publish
 * the cell size. build_end restores 512 once the build has finished. */
void sh_world_area_cell_build(void *map);
void sh_world_area_cell_build_end(void);
/* A map was opened; the editor tick starts counting its areas. */
void sh_world_area_cell_map_opened(void);
void sh_world_area_cell_editor_tick(void);
#endif
#endif
