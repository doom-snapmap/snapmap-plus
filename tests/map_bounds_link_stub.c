/* Link stubs for tests that compile rawmap.c or nav_play.c directly: the map
 * bounds hooks those files call are exercised by map_bounds_test instead, and
 * the Play-time requirements check by modserve_test, the Script node gate by
 * script_node_test (no map here opens scripts). */
void sh_map_bounds_loaded(void *snapshot){(void)snapshot;}
void sh_map_bounds_build(void *map){(void)map;}
void sh_world_area_cell_build(void *map){(void)map;}
void sh_world_area_cell_build_end(void){}
void sh_world_area_cell_map_opened(void){}
void sh_modserve_play_begin(void){}
#include <stddef.h>
int sh_script_hooks_open(void){return 0;}
int sh_apply_engine_map_json(const void *map, char **out, size_t *len){(void)map;*out=NULL;*len=0;return 0;}
void sh_script_node_play_begin(const char *json, size_t len){(void)json;(void)len;}
