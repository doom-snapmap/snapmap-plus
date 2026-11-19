/* saved_maps.h -- the player's local saved maps (My Maps), read from disk.
 *
 * The game keeps one slot folder per local saved map,
 *
 *     <Saved Games>\id Software\DOOM\base\savegame.user\<profile>\SNAPMAPS<local id>\
 *
 * holding game.details (key=value lines: checksum=, itemId=, metadata=<JSON>, modified=) and map.decl (a 4-byte
 * checksum, then zlib: a 2-byte header, raw DEFLATE, a 4-byte Adler-32, of the map's JSON). A save made offline
 * exists only here; the game's doomsnapmaps\saved-maps folder holds the cloud copies of maps saved online. This file
 * is the one reader of the slots: the Mods tab (a slot's name and time, the saves that use a mod) and the content
 * cache (the units a kept map uses) read them through it. Pure file work, any thread. */
#ifndef SNAPMAP_PLUS_SAVED_MAPS_H
#define SNAPMAP_PLUS_SAVED_MAPS_H

#include <stddef.h>

/* The savegame.user folder (malloc'd), or NULL when the Saved Games folder is unknown. */
char *sh_saved_maps_root(void);
/* Read the slots under `path` instead (tests, tools); NULL restores the game's folder. */
void  sh_saved_maps_set_root(const char *path);

/* The map JSON of a map.decl (malloc'd, NUL-terminated, its length in *len), or NULL when the file is absent or
 * does not decode. */
char *sh_saved_map_text(const char *path, size_t *len);

/* One slot: its local id (the folder's suffix), the cloud-saved item id its game.details names ("" for none), and
 * the map's JSON. */
typedef void (*sh_saved_map_fn)(void *context, const char *local_id, const char *item_id, const char *text, size_t len);

/* Every slot of every profile whose map.decl decodes, to `fn`. Returns the number of maps read; 0 when the
 * savegame.user folder does not exist (no saved maps); -1 when it exists and cannot be listed, or the Saved Games
 * folder is unknown. *unreadable (optional) counts the slots whose map.decl is there and does not decode: a caller
 * that must never lose what a kept map uses treats any as unknown. A slot without map.decl (a deleted map leaves
 * its .verify sidecars) is not a map. */
int   sh_saved_maps_each(sh_saved_map_fn fn, void *context, size_t *unreadable);

/* The game.details text of the slot `local_id` under any profile (malloc'd), or NULL. The id must be the engine's
 * own: letters and digits only, at most 40. */
char *sh_saved_map_details(const char *local_id);

#endif /* SNAPMAP_PLUS_SAVED_MAPS_H */
