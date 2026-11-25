/* saved_maps.h -- the one reader of the player's local saved maps (My Maps); pure file work, any thread.
 * Each slot folder <Saved Games>\id Software\DOOM\base\savegame.user\<profile>\SNAPMAPS<id>\ holds game.details
 * (key=value lines) and map.decl (a 4-byte checksum, then the map's JSON as zlib). */
#ifndef SNAPMAP_PLUS_SAVED_MAPS_H
#define SNAPMAP_PLUS_SAVED_MAPS_H

#include <stddef.h>

/* The savegame.user folder (malloc'd), or NULL when the Saved Games folder is unknown. */
char *sh_saved_maps_root(void);
/* Read the slots under `path` instead (tests, tools); NULL restores the game's folder. */
void  sh_saved_maps_set_root(const char *path);


/* One slot: its local id (the folder's suffix), the cloud-saved item id its game.details names ("" for none), and
 * the map's JSON. */
typedef void (*sh_saved_map_fn)(void *context, const char *local_id, const char *item_id, const char *text, size_t len);

/* Calls `fn` for every slot of every profile whose map.decl decodes. Returns the number of maps read, 0 when
 * savegame.user does not exist, or -1 when it cannot be listed or the Saved Games folder is unknown. *unreadable
 * (optional) counts slots whose map.decl exists but does not decode; a slot without map.decl is not a map. */
int   sh_saved_maps_each(sh_saved_map_fn fn, void *context, size_t *unreadable);

/* The game.details text of the slot `local_id` under any profile (malloc'd), or NULL. The id must be the engine's
 * own: letters and digits only, at most 40. */
char *sh_saved_map_details(const char *local_id);

#endif /* SNAPMAP_PLUS_SAVED_MAPS_H */
