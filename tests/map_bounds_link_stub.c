/* Link stubs for tests that compile rawmap.c or nav_play.c directly: the map
 * bounds hooks those files call are exercised by map_bounds_test instead. */
void sh_map_bounds_loaded(void *snapshot){(void)snapshot;}
void sh_map_bounds_build(void *map){(void)map;}
