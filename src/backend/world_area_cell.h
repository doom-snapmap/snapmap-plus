/* Play's SnapMap world-area cell size, chosen per map.
 *
 * BuildRenderWorld divides the map into world areas at most 512 units wide,
 * and the engine refuses a map with more than 8192 areas or area nodes (its
 * loading screen then never returns). The 8192 is structural: the render
 * frontend sizes per-frame culling buffers by it and stores area indices as
 * 16-bit values. A map that needs more areas at 512 units is built with the
 * smallest coarser size (1024, 2048, ...) that fits instead; every map that
 * fits at 512 keeps the native size. */
#ifndef SH_WORLD_AREA_CELL_H
#define SH_WORLD_AREA_CELL_H
#include <stdint.h>
#include "signatures.h"

/* The BuildRenderWorld instruction that loads the cell size for the
 * subdivision call: MOVSS xmm0,[rip+disp32] then MOVSS [rsp+0x20],xmm0
 * (the call's fifth argument). */
#define SH_WORLD_AREA_CELL_SITE_LEN 14

/* Check the site's bytes; on success *constant is the address its disp32
 * reads. Does not read the constant. */
int sh_world_area_cell_site(const unsigned char *site,uintptr_t *constant);
/* The disp32 that makes the site read dest; 0 when dest is out of reach. */
int sh_world_area_cell_disp(uintptr_t site,uintptr_t dest,int32_t *disp);

#ifndef SH_WORLD_AREA_CELL_PURE
/* Boot thread, after the Grid Room native install. Logs either way. */
int sh_world_area_cell_install(const sig_result *results,size_t count,const uint8_t *base);
/* Whether the redirect is live. */
int sh_world_area_cell_live(void);
/* The SnapBuild detour, before the native build: count the map and publish
 * the cell size. build_end restores 512 once the build has finished. */
void sh_world_area_cell_build(void *map);
void sh_world_area_cell_build_end(void);
/* A map was opened; the editor-frame tick logs its cell size once it is loaded. */
void sh_world_area_cell_map_opened(void);
void sh_world_area_cell_editor_tick(void);
/* The value the site currently reads (512 when not live). */
float sh_world_area_cell_current(void);
#endif
#endif
