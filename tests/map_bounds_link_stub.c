/* Link stubs for tests that compile rawmap.c or nav_play.c directly: the map
 * bounds hooks those files call are exercised by map_bounds_test instead, and
 * the Play-time requirements check by modserve_test. */
void sh_map_bounds_loaded(void *snapshot){(void)snapshot;}
void sh_map_bounds_build(void *map){(void)map;}
void sh_world_area_cell_build(void *map){(void)map;}
void sh_world_area_cell_build_end(void){}
void sh_world_area_cell_map_opened(void){}
void sh_modserve_play_begin(void){}
